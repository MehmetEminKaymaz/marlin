#include "NativeCodegen.h"

#include <cstdlib>

#include "llvm/IR/LegacyPassManager.h"
#include "llvm/MC/TargetRegistry.h"
#include "llvm/Passes/PassBuilder.h"
#include "llvm/Support/CodeGen.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/Program.h"
#include "llvm/Support/TargetSelect.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/TargetParser/Host.h"
#include "llvm/TargetParser/Triple.h"

namespace marlin {

std::unique_ptr<NativeCodegen> NativeCodegen::create(unsigned optLevel, std::string& error) {
    llvm::InitializeNativeTarget();
    llvm::InitializeNativeTargetAsmPrinter();

    const std::string triple = llvm::sys::getDefaultTargetTriple();
    const llvm::Target* target = llvm::TargetRegistry::lookupTarget(triple, error);
    if (!target) return nullptr;

    static const llvm::CodeGenOptLevel levels[] = {
        llvm::CodeGenOptLevel::None, llvm::CodeGenOptLevel::Less,
        llvm::CodeGenOptLevel::Default, llvm::CodeGenOptLevel::Aggressive};
    if (optLevel > 3) optLevel = 3;

    // PIC: günümüz Linux dağıtımları ve macOS varsayılan olarak konumdan
    // bağımsız çalıştırılabilir (PIE) bağlar; Windows'ta (COFF) etkisizdir.
    std::unique_ptr<llvm::TargetMachine> machine(target->createTargetMachine(
        triple, "generic", "", llvm::TargetOptions(), llvm::Reloc::PIC_,
        std::nullopt, levels[optLevel]));
    if (!machine) {
        error = "hedef makine olusturulamadi: " + triple;
        return nullptr;
    }
    return std::unique_ptr<NativeCodegen>(new NativeCodegen(std::move(machine), optLevel));
}

void NativeCodegen::configure(llvm::Module& module) {
    module.setTargetTriple(targetMachine->getTargetTriple().str());
    module.setDataLayout(targetMachine->createDataLayout());
}

// -O0'da da LLVM'in "O0 hattı" çalıştırılır: optimizasyon yapmaz, ama paralel
// döngülerin kullandığı coroutine'leri sıradan fonksiyonlara indirir; kod
// üretici coroutine intrinsic'lerini doğrudan derleyemez.
void NativeCodegen::optimize(llvm::Module& module) {
    static const llvm::OptimizationLevel levels[] = {
        llvm::OptimizationLevel::O0, llvm::OptimizationLevel::O1,
        llvm::OptimizationLevel::O2, llvm::OptimizationLevel::O3};

    llvm::PassBuilder passBuilder(targetMachine.get());
    llvm::LoopAnalysisManager loopAM;
    llvm::FunctionAnalysisManager functionAM;
    llvm::CGSCCAnalysisManager cgsccAM;
    llvm::ModuleAnalysisManager moduleAM;
    passBuilder.registerModuleAnalyses(moduleAM);
    passBuilder.registerCGSCCAnalyses(cgsccAM);
    passBuilder.registerFunctionAnalyses(functionAM);
    passBuilder.registerLoopAnalyses(loopAM);
    passBuilder.crossRegisterProxies(loopAM, functionAM, cgsccAM, moduleAM);

    llvm::ModulePassManager pipeline = optLevel == 0
        ? passBuilder.buildO0DefaultPipeline(llvm::OptimizationLevel::O0)
        : passBuilder.buildPerModuleDefaultPipeline(levels[optLevel]);
    pipeline.run(module, moduleAM);
}

bool NativeCodegen::emitObject(llvm::Module& module, const std::string& path,
                               std::string& error) {
    std::error_code ec;
    llvm::raw_fd_ostream out(path, ec, llvm::sys::fs::OF_None);
    if (ec) {
        error = "nesne dosyasi acilamadi: " + path + " (" + ec.message() + ")";
        return false;
    }

    // Kod üretimi hâlâ eski (legacy) pass yöneticisini kullanır.
    llvm::legacy::PassManager codegenPasses;
    if (targetMachine->addPassesToEmitFile(codegenPasses, out, nullptr,
                                           llvm::CodeGenFileType::ObjectFile)) {
        error = "hedef makine nesne dosyasi uretemiyor";
        return false;
    }
    codegenPasses.run(module);
    out.flush();
    return true;
}

// Derleyicinin çalıştığı platform; program da onun için üretilir.
static llvm::Triple hostTriple() { return llvm::Triple(llvm::sys::getDefaultTargetTriple()); }

const char* NativeCodegen::runtimeLibraryName() {
    return hostTriple().isWindowsMSVCEnvironment() ? "marlinrt.lib" : "libmarlinrt.a";
}

const char* NativeCodegen::defaultExecutableName() {
    return hostTriple().isOSWindows() ? "a.exe" : "a.out";
}

std::string NativeCodegen::findRuntimeLibrary(const char* argv0) {
    if (const char* fromEnv = std::getenv("MARLIN_RUNTIME"))
        return llvm::sys::fs::exists(fromEnv) ? fromEnv : "";

    // Adresi alınan sembol yalnızca çalışan dosyanın yerini bulmak içindir.
    static int anchor;
    llvm::SmallString<256> beside(llvm::sys::fs::getMainExecutable(argv0, &anchor));
    llvm::sys::path::remove_filename(beside);
    llvm::sys::path::append(beside, runtimeLibraryName());
    if (llvm::sys::fs::exists(beside)) return std::string(beside);

#ifdef MARLIN_RUNTIME_LIB
    if (llvm::sys::fs::exists(MARLIN_RUNTIME_LIB)) return MARLIN_RUNTIME_LIB;
#endif
    return "";
}

// Paylaşımlı kütüphane mi? (Linux .so / .so.N, macOS .dylib)
static bool isSharedLibrary(llvm::StringRef path) {
    return path.contains(".so") || path.ends_with(".dylib");
}

// MLIR'in çalışma zamanı kütüphanesi (iş parçacığı havuzu, CUDA sarmalayıcısı):
// paylaşımlı bir kütüphanedir. POSIX'te yolu çalıştırılabilir dosyaya yazılır
// (rpath); Windows'ta rpath yoktur, DLL çalıştırılabilir dosyanın yanına
// kopyalanır (Windows DLL'i önce orada arar).
static bool addMlirRuntime(const llvm::Triple& triple, llvm::StringRef name, const std::string& exePath,
                           std::vector<std::string>& args, std::string& error) {
#ifdef MARLIN_LLVM_LIB_DIR
    if (triple.isOSWindows()) {
        args.push_back((llvm::Twine(MARLIN_LLVM_LIB_DIR) + "/" + name + ".lib").str());
#ifdef MARLIN_LLVM_BIN_DIR
        const std::string dll = (llvm::Twine(MARLIN_LLVM_BIN_DIR) + "/" + name + ".dll").str();
        llvm::SmallString<256> target(exePath);
        llvm::sys::path::remove_filename(target);
        llvm::sys::path::append(target, name + ".dll");
        if (std::error_code ec = llvm::sys::fs::copy_file(dll, target)) {
            error = dll + " kopyalanamadi: " + ec.message();
            return false;
        }
#endif
        return true;
    }
    // Dosya tam yoluyla verilir: bazı dağıtımlarda (conda) yalnızca sürüm
    // numaralı adı vardır (libmlir_async_runtime.so.19.1, .19.1.dylib) ve
    // -lmlir_async_runtime onu bulamaz.
    const std::string prefix = ("lib" + name).str();
    std::string found;
    std::error_code ec;
    for (llvm::sys::fs::directory_iterator it(MARLIN_LLVM_LIB_DIR, ec), end; it != end && !ec; it.increment(ec)) {
        llvm::StringRef file = llvm::sys::path::filename(it->path());
        if (!file.starts_with(prefix + ".") || !isSharedLibrary(file)) continue;
        // Sürümsüz ad (libX.so, libX.dylib) varsa o tercih edilir.
        if (found.empty() || file == prefix + ".so" || file == prefix + ".dylib") found = it->path();
    }
    if (found.empty()) {
        error = (name + " bulunamadi (" + MARLIN_LLVM_LIB_DIR + ")").str();
        return false;
    }
    args.push_back(found);
    args.push_back(std::string("-Wl,-rpath,") + MARLIN_LLVM_LIB_DIR);
    return true;
#else
    (void)triple, (void)exePath, (void)args;
    error = (name + " bulunamadi: derleyici MARLIN_LLVM_LIB_DIR olmadan derlenmis").str();
    return false;
#endif
}

// Bağlayıcı olarak C derleyicisinin sürücüsü çağrılır (cc / clang / gcc): o,
// platformun sistem kütüphanelerini ve başlangıç dosyalarını kendisi ekler.
// Windows'ta clang (MSVC hedefinde lld-link ya da link.exe'yi çağırır).
bool NativeCodegen::link(const std::string& objectPath, const std::string& runtimeLib,
                         const std::string& exePath, bool needsThreads, bool needsGpu,
                         const std::vector<std::string>& extraLibs, std::string& error) {
    const llvm::Triple triple = hostTriple();
    const bool windows = triple.isOSWindows();

    llvm::ErrorOr<std::string> linker = std::make_error_code(std::errc::no_such_file_or_directory);
    const std::vector<const char*> candidates =
        windows ? std::vector<const char*>{"clang"} : std::vector<const char*>{"cc", "clang", "gcc"};
    for (const char* name : candidates) {
        linker = llvm::sys::findProgramByName(name);
        if (linker) break;
    }
    if (!linker) {
        error = windows ? "baglayici bulunamadi (clang PATH'te olmali)"
                        : "baglayici bulunamadi (cc, clang ya da gcc PATH'te olmali)";
        return false;
    }

    // Sıra önemli: kütüphaneler, onları kullanan nesne dosyasından sonra gelir.
    std::vector<std::string> args = {*linker, objectPath, runtimeLib, "-o", exePath};

    // Standart kütüphanenin dış C kütüphaneleri (veritabanı sürücüleri), tam
    // yollarıyla. Paylaşımlı kütüphanenin dizini çalışırken de aranır (rpath):
    // sisteme kurulmadan açılmış bir kütüphane de bulunabilsin.
    std::vector<std::string> runPaths;
    for (const std::string& lib : extraLibs) {
        args.push_back(lib);
        if (!windows && isSharedLibrary(lib))
            runPaths.push_back("-Wl,-rpath," + llvm::sys::path::parent_path(lib).str());
    }
    args.insert(args.end(), runPaths.begin(), runPaths.end());

    // Paralel döngüler ve görevler MLIR'in iş parçacığı havuzunu kullanır.
    // GPU'da çalışan kod, GPU ile MLIR'in CUDA sarmalayıcısı üzerinden konuşur
    // (bellek ayırma, kopyalama, çekirdek başlatma); o da sistemdeki NVIDIA
    // sürücüsünü (libcuda) kullanır.
    if (needsThreads && !addMlirRuntime(triple, "mlir_async_runtime", exePath, args, error)) return false;
    if (needsGpu && !addMlirRuntime(triple, "mlir_cuda_runtime", exePath, args, error)) return false;

    // Sistem kütüphaneleri en sonda (statik bağlanan dış kütüphaneler de kullanabilir):
    //   POSIX: libm (sin, pow, kayan noktalı % gibi işlemler); macOS'ta libSystem'in
    //          parçasıdır, -lm yine de geçerlidir.
    //   Windows: matematik C kütüphanesindedir; soketler ws2_32, argümanlar shell32.
    if (windows) {
        args.push_back("-lws2_32");
        args.push_back("-lshell32");
        // C kütüphanesinin DLL hali (msvcrt/ucrt): runtime /MD ile derlenir,
        // MLIR'in DLL'leri de onu kullanır. clang'ın varsayılanı statik
        // libcmt'dir; ikisi karışırsa bağlayıcı çakışır.
        args.push_back("-fms-runtime-lib=dll");
    } else {
        args.push_back("-lm");
    }

    std::vector<llvm::StringRef> argRefs(args.begin(), args.end());
    int status = llvm::sys::ExecuteAndWait(*linker, argRefs, std::nullopt, {}, 0, 0, &error);
    if (status != 0) {
        if (error.empty()) error = "baglama basarisiz (cikis kodu " + std::to_string(status) + ")";
        return false;
    }
    return true;
}

} // namespace marlin
