#include <iostream>
#include <string>
#include <vector>

#include "AST/ASTPrinter.h"
#include "AST/ModuleLoader.h"
#include "AST/Sema.h"
#include "MLIRBackend/MLIRGenerator.h"
#include "LLVMBackend/LLVMLowering.h"
#include "LLVMBackend/NativeCodegen.h"
#include "Lsp/MarlinLsp.h"
#include "Tools/Doctor.h"

#include "llvm/ADT/SmallString.h"
#include "llvm/Config/llvm-config.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/TargetParser/Host.h"
#include "mlir/Dialect/Arith/IR/Arith.h"
#include "mlir/Dialect/ControlFlow/IR/ControlFlow.h"
#include "mlir/Dialect/Func/IR/FuncOps.h"
#include "mlir/Dialect/GPU/IR/GPUDialect.h"
#include "mlir/Dialect/LLVMIR/LLVMDialect.h"
#include "mlir/Dialect/Affine/IR/AffineOps.h"
#include "mlir/Dialect/Async/IR/Async.h"
#include "mlir/Dialect/Bufferization/IR/Bufferization.h"
#include "mlir/Dialect/Linalg/IR/Linalg.h"
#include "mlir/Dialect/Math/IR/Math.h"
#include "mlir/Dialect/Tensor/IR/Tensor.h"
#include "mlir/Dialect/MemRef/IR/MemRef.h"
#include "mlir/Dialect/SCF/IR/SCF.h"
#include "mlir/IR/Builders.h"
#include "mlir/IR/BuiltinOps.h"
#include "mlir/IR/MLIRContext.h"
#include "mlir/IR/Verifier.h"
#include "mlir/Target/LLVMIR/Dialect/All.h"

// Dosya verilmezse derlenen örnek program.
static const char* const kSampleSource = R"(
    int globalX = 42;
    float globalPi = 3.14;

    // Basit tür çıkarımı ve temel işlemler
    func main() -> void {
        let a = 10;                 // let ile tür çıkarımı (int)
        int b = 20;
        let sonuc = a + b * 2;      // binary expr + let
        bool flag = true;

        // if-elseif-else zinciri
        if sonuc > 50 {
            sonuc = sonuc - 5;
        } else if sonuc == 50 {
            sonuc = 0;
        } else {
            sonuc = 1;
        }

        // Dizi tanımlama ve dizi erişimi
        int[5] dizi = [1, 2, 3, 4, 5];
        dizi[2] = 99;

        // for-range döngüsü (break/continue içerebilir)
        int toplam = 0;
        for i in 0 .. 10 step 2 {
            if i == 6 {
                continue;
            }
            toplam = toplam + i;
            if toplam > 20 {
                break;
            }
        }

        // for-collection döngüsü
        for sayi in dizi {
            toplam = toplam + sayi;
        }

        // while döngüsü
        while toplam < 500 {
            toplam = toplam + 1;
        }

        // Lambda ifadesi (hemen çağrı)
        let kareAl = (int x) -> int | x * x;
        let kareSonuc = kareAl(5);   // fonksiyon çağrısı

        // Fonksiyon çağrısı (başka bir func)
        yardimciFonksiyon(toplam);
    }

    func yardimciFonksiyon(int girdi) -> void {
        int sonuc = girdi * 2;
        // string kullanımı
        string mesaj = "merhaba";
    }
)";

static const char* const kUsage =
    "Kullanim: marlin_lang [kaynak.marl] [secenekler]\n"
    "          marlin_lang doctor   kurulumu denetler (baglayici, GPU, kutuphaneler)\n"
    "  kaynak.marl   derlenecek dosya (verilmezse gomulu ornek program)\n"
    "  -o <dosya>     cikti dosyasi (varsayilan: exe icin a.out, obj icin a.o,\n"
    "                 metin ciktilari icin ekran)\n"
    "  -I <dizin>     import edilen modullerin aranacagi ek dizin (birden cok\n"
    "                 kez verilebilir; once import eden dosyanin dizinine bakilir)\n"
    "  --emit=<tur>   hattin hangi asamasinda durulacagi:\n"
    "                   mlir       ust seviye MLIR (scf, memref, arith ...)\n"
    "                   gpu-mlir   GPU cekirdekleri ayrilmis MLIR (gpu.module)\n"
    "                   llvm-mlir  llvm dialect'ine indirilmis MLIR\n"
    "                   llvm-ir    LLVM IR (.ll)\n"
    "                   obj        nesne dosyasi (.o)\n"
    "                   exe        calistirilabilir dosya (varsayilan)\n"
    "  -O0 .. -O3     LLVM optimizasyon seviyesi (varsayilan: -O0)\n"
    "  --gpu=auto     (varsayilan) @GPU fonksiyonlar GPU icin de derlenir; program\n"
    "                 calisirken GPU varsa onu (NVIDIA: CUDA, diger: Vulkan), yoksa\n"
    "                 CPU kopyasini kullanir. NVIDIA cekirdekleri derleme\n"
    "                 makinesinde CUDA arac takimi varsa uretilir.\n"
    "  --gpu=on       auto gibi; CUDA arac takimi yoksa uyarir\n"
    "  --gpu=off      @GPU fonksiyonlari yalnizca CPU icin derler\n"
    "  --print-ast    semantik analizden gecmis AST'yi yazdirir\n"
    "  --lsp          dil sunucusu olarak calisir (editor eklentisi icin; tek basina kullanilir)\n"
    "  --version      derleyicinin surumunu yazar\n";

enum class EmitKind { MLIR, GpuMLIR, LLVMDialect, LLVMIR, Object, Executable };

// Metin çıktısını dosyaya ya da (yol boşsa) ekrana yazar.
template <typename PrintFn>
static bool writeText(const std::string& path, PrintFn print) {
    if (path.empty()) {
        print(llvm::outs());
        llvm::outs() << "\n";
        return true;
    }
    std::error_code ec;
    llvm::raw_fd_ostream out(path, ec, llvm::sys::fs::OF_Text);
    if (ec) {
        std::cerr << "Cikti dosyasi acilamadi: " << path << "\n";
        return false;
    }
    print(out);
    out << "\n";
    return true;
}

int main(int argc, char** argv) {
    // Dil sunucusu: editörle (VS Code eklentisi) stdin / stdout üzerinden konuşur.
    if (argc >= 2 && std::string(argv[1]) == "--lsp") {
        static int lspAnchor;
        return marlin::runLanguageServer(llvm::sys::fs::getMainExecutable(argv[0], &lspAnchor));
    }
    if (argc == 2 && std::string(argv[1]) == "doctor") return marlin::runDoctor(argv[0]);

    std::string inputPath, outputPath;
    std::vector<std::string> importDirs;
    EmitKind emit = EmitKind::Executable;
    unsigned optLevel = 0;
    bool printAst = false;
    enum class GpuMode { Auto, On, Off } gpuMode = GpuMode::Auto;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "-o" && i + 1 < argc) outputPath = argv[++i];
        else if (arg == "-I" && i + 1 < argc) importDirs.push_back(argv[++i]);
        else if (arg == "--print-ast") printAst = true;
        else if (arg == "--gpu=off") gpuMode = GpuMode::Off;
        else if (arg == "--gpu=on") gpuMode = GpuMode::On;
        else if (arg == "--gpu=auto") gpuMode = GpuMode::Auto;
        else if (arg == "-O0" || arg == "-O1" || arg == "-O2" || arg == "-O3") optLevel = arg[2] - '0';
        else if (arg == "--emit=mlir") emit = EmitKind::MLIR;
        else if (arg == "--emit=gpu-mlir") emit = EmitKind::GpuMLIR;
        else if (arg == "--emit=llvm-mlir") emit = EmitKind::LLVMDialect;
        else if (arg == "--emit=llvm-ir") emit = EmitKind::LLVMIR;
        else if (arg == "--emit=obj") emit = EmitKind::Object;
        else if (arg == "--emit=exe") emit = EmitKind::Executable;
        else if (arg == "-h" || arg == "--help") { std::cout << kUsage; return 0; }
        else if (arg == "--version") {
            std::cout << "marlin " << MARLIN_VERSION << " (LLVM " << LLVM_VERSION_STRING << ", "
                      << llvm::sys::getDefaultTargetTriple() << ")\n";
            return 0;
        }
        else if (arg[0] == '-') { std::cerr << "Bilinmeyen secenek: " << arg << "\n" << kUsage; return 1; }
        else inputPath = arg;
    }

    // --- Ayrıştırma ve AST ---
    // Derlenen dosya ile import ettiği bütün dosyalar ayrıştırılır ve tek bir
    // programda birleştirilir.
    static int anchor; // adresi yalnızca çalışan dosyanın yerini bulmak içindir
    ModuleLoader loader(importDirs,
                        ModuleLoader::findStdDir(llvm::sys::fs::getMainExecutable(argv[0], &anchor)));
    std::unique_ptr<Program> astRoot = inputPath.empty() ? loader.loadSource(kSampleSource, "<ornek>")
                                                         : loader.load(inputPath);
    if (!astRoot) {
        for (const auto& err : loader.getErrors()) std::cerr << err << "\n";
        return 1;
    }

    // PostgreSQL sürücüsü ve HTTP istemcisi dış bir C kütüphanesi kullanır.
    // Program ona bağlanmaz, çalışırken yükler (runtime/marlin_library.c);
    // Windows'ta kütüphane sistemde bulunmadığı için DLL'i programın yanına
    // konur (bkz. NativeCodegen::link).
    std::vector<std::string> sideLibraries;
    for (const Module& module : astRoot->modules) {
        const char* dll = nullptr;
        const char* missing = nullptr;
        if (module.name == "std.postgres") {
#ifdef MARLIN_HAVE_POSTGRES
            dll = "libpq.dll";
#else
            missing = "libpq (paket: libpq-dev)";
#endif
        } else if (module.name == "std.http.client") {
#ifdef MARLIN_HAVE_CURL
            dll = "libcurl.dll";
#else
            missing = "libcurl (paket: libcurl4-openssl-dev)";
#endif
        }
        if (missing) {
            std::cerr << module.name << " kullanilamiyor: derleyici " << missing
                      << " basliklari olmadan derlenmis; kutuphaneyi kurup derleyiciyi yeniden derleyin\n";
            return 1;
        }
        if (dll) sideLibraries.push_back(dll);
    }

    // --- Semantik analiz ---
    // Tipleri çözer, örtük dönüşümleri AST'ye yazar ve akış yapısını işaretler.
    // Kod üretimi geçerli bir ağaç varsaydığı için hata varsa burada durulur.
    Sema sema;
    astRoot->accept(sema);
    if (sema.hasErrors()) {
        for (const auto& err : sema.getErrors()) std::cerr << err << "\n";
        return 1;
    }

    if (printAst) {
        ASTPrinter printer;
        astRoot->accept(printer);
        std::cout.flush(); // sonraki çıktılar llvm::outs ile yazılıyor; sıra karışmasın
    }

    // --- MLIR üretimi ---
    mlir::DialectRegistry registry;
    registry.insert<mlir::func::FuncDialect>();
    registry.insert<mlir::arith::ArithDialect>();
    registry.insert<mlir::math::MathDialect>();
    registry.insert<mlir::scf::SCFDialect>();
    registry.insert<mlir::cf::ControlFlowDialect>();
    registry.insert<mlir::LLVM::LLVMDialect>();
    registry.insert<mlir::memref::MemRefDialect>();
    registry.insert<mlir::tensor::TensorDialect>();
    registry.insert<mlir::linalg::LinalgDialect>();
    registry.insert<mlir::bufferization::BufferizationDialect>();
    registry.insert<mlir::async::AsyncDialect>();   // görevler (marl) ve paralel döngüler
    registry.insert<mlir::affine::AffineDialect>();
    registry.insert<mlir::gpu::GPUDialect>();       // @GPU fonksiyonlar
    marlin::registerLoweringInterfaces(registry);
    mlir::registerAllToLLVMIRTranslations(registry);

    // Registry context'e verilir; LLVM IR'a çeviri arayüzleri ancak böyle bulunur.
    mlir::MLIRContext mlirCtx(registry);
    mlirCtx.getOrLoadDialect<mlir::func::FuncDialect>();
    mlirCtx.getOrLoadDialect<mlir::arith::ArithDialect>();
    mlirCtx.getOrLoadDialect<mlir::math::MathDialect>();
    mlirCtx.getOrLoadDialect<mlir::scf::SCFDialect>();
    mlirCtx.getOrLoadDialect<mlir::cf::ControlFlowDialect>();
    mlirCtx.getOrLoadDialect<mlir::memref::MemRefDialect>();
    mlirCtx.getOrLoadDialect<mlir::LLVM::LLVMDialect>();
    mlirCtx.getOrLoadDialect<mlir::tensor::TensorDialect>();
    mlirCtx.getOrLoadDialect<mlir::linalg::LinalgDialect>();
    mlirCtx.getOrLoadDialect<mlir::bufferization::BufferizationDialect>();
    mlirCtx.getOrLoadDialect<mlir::gpu::GPUDialect>();
    mlirCtx.getOrLoadDialect<mlir::async::AsyncDialect>();

    mlir::OpBuilder mlirBuilder(&mlirCtx);
    mlir::ModuleOp moduleOp = mlir::ModuleOp::create(mlirBuilder.getUnknownLoc());

    // @GPU fonksiyonların çekirdekleri Vulkan (SPIR-V) için her zaman,
    // NVIDIA (PTX) için derleme makinesinde CUDA araç takımı varsa derlenir.
    // Derlenen program her makinede çalışır: uygun GPU yoksa CPU ikizini
    // kullanır. --gpu=off: yalnızca CPU.
    const marlin::GpuToolchain gpuToolchain =
        gpuMode == GpuMode::Off ? marlin::GpuToolchain{} : marlin::findGpuToolchain();
    if (gpuMode == GpuMode::On && !gpuToolchain.cuda)
        std::cerr << "uyari: NVIDIA cekirdekleri derlenmeyecek (" << gpuToolchain.missing
                  << "); NVIDIA kartta da Vulkan kullanilacak\n";

    MLIRGenerator generator(mlirCtx, mlirBuilder, moduleOp);
    generator.setGpuEnabled(gpuMode != GpuMode::Off);
    astRoot->accept(generator);

    // Üretilen modülü doğrula: kod üretimindeki hatalar (eksik terminator, tip
    // uyuşmazlığı...) ilerideki bir pass'te anlaşılmaz bir çökme yerine burada,
    // net bir mesajla yakalanır.
    if (mlir::failed(mlir::verify(moduleOp))) {
        llvm::errs() << "Uretilen MLIR modulu dogrulamadan gecemedi!\n";
        moduleOp->dump();
        return 1;
    }

    if (emit == EmitKind::MLIR)
        return writeText(outputPath, [&](llvm::raw_ostream& os) { moduleOp->print(os); }) ? 0 : 1;

    // --- MLIR → llvm dialect'i ---
    if (!marlin::lowerToLLVMDialect(moduleOp, generator.usesGpuCode() ? &gpuToolchain : nullptr,
                                   /*stopAfterGpuSplit=*/emit == EmitKind::GpuMLIR)) {
        llvm::errs() << "MLIR, LLVM dialect'ine indirilemedi!\n";
        return 1;
    }
    if (emit == EmitKind::GpuMLIR || emit == EmitKind::LLVMDialect)
        return writeText(outputPath, [&](llvm::raw_ostream& os) { moduleOp->print(os); }) ? 0 : 1;

    // --- llvm dialect'i → LLVM IR ---
    llvm::LLVMContext llvmContext;
    std::unique_ptr<llvm::Module> llvmModule =
        marlin::translateToLLVMIR(moduleOp, llvmContext, generator.usesTaskCode());
    if (!llvmModule) {
        llvm::errs() << "LLVM IR'a ceviri basarisiz!\n";
        return 1;
    }

    // --- Optimizasyon ve makine kodu ---
    std::string error;
    std::unique_ptr<marlin::NativeCodegen> codegen = marlin::NativeCodegen::create(optLevel, error);
    if (!codegen) {
        llvm::errs() << "Kod uretici hazirlanamadi: " << error << "\n";
        return 1;
    }
    codegen->configure(*llvmModule);
    codegen->optimize(*llvmModule);

    if (emit == EmitKind::LLVMIR)
        return writeText(outputPath, [&](llvm::raw_ostream& os) { llvmModule->print(os, nullptr); }) ? 0 : 1;

    if (emit == EmitKind::Object) {
        if (!codegen->emitObject(*llvmModule, outputPath.empty() ? "a.o" : outputPath, error)) {
            llvm::errs() << error << "\n";
            return 1;
        }
        return 0;
    }

    // --- Bağlama: geçici nesne dosyası + çalışma zamanı → çalıştırılabilir ---
    std::string runtimeLib = marlin::NativeCodegen::findRuntimeLibrary(argv[0]);
    if (runtimeLib.empty()) {
        llvm::errs() << "Calisma zamani kutuphanesi (" << marlin::NativeCodegen::runtimeLibraryName()
                     << ") bulunamadi; MARLIN_RUNTIME ortam degiskeniyle yolunu verin.\n";
        return 1;
    }
    llvm::SmallString<128> objectPath;
    if (std::error_code ec = llvm::sys::fs::createTemporaryFile("marlin", "o", objectPath)) {
        llvm::errs() << "Gecici dosya olusturulamadi: " << ec.message() << "\n";
        return 1;
    }
    bool ok = codegen->emitObject(*llvmModule, objectPath.str().str(), error) &&
              marlin::NativeCodegen::link(objectPath.str().str(), runtimeLib,
                                         outputPath.empty() ? marlin::NativeCodegen::defaultExecutableName()
                                                            : outputPath,
                                         generator.usesGpuCode(),
                                         sideLibraries, error);
    llvm::sys::fs::remove(objectPath);
    if (!ok) {
        llvm::errs() << error << "\n";
        return 1;
    }
    return 0;
}
