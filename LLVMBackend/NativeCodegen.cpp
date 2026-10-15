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

    // PIC: günümüz Linux dağıtımları varsayılan olarak konumdan bağımsız
    // çalıştırılabilir (PIE) bağlar.
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

std::string NativeCodegen::findRuntimeLibrary(const char* argv0) {
    if (const char* fromEnv = std::getenv("MARLIN_RUNTIME"))
        return llvm::sys::fs::exists(fromEnv) ? fromEnv : "";

    // Adresi alınan sembol yalnızca çalışan dosyanın yerini bulmak içindir.
    static int anchor;
    llvm::SmallString<256> beside(llvm::sys::fs::getMainExecutable(argv0, &anchor));
    llvm::sys::path::remove_filename(beside);
    llvm::sys::path::append(beside, "libmarlinrt.a");
    if (llvm::sys::fs::exists(beside)) return std::string(beside);

#ifdef MARLIN_RUNTIME_LIB
    if (llvm::sys::fs::exists(MARLIN_RUNTIME_LIB)) return MARLIN_RUNTIME_LIB;
#endif
    return "";
}

bool NativeCodegen::link(const std::string& objectPath, const std::string& runtimeLib,
                         const std::string& exePath, bool needsThreads, bool needsGpu,
                         std::string& error) {
    llvm::ErrorOr<std::string> linker = std::make_error_code(std::errc::no_such_file_or_directory);
    for (const char* name : {"cc", "clang", "gcc"}) {
        linker = llvm::sys::findProgramByName(name);
        if (linker) break;
    }
    if (!linker) {
        error = "baglayici bulunamadi (cc, clang ya da gcc PATH'te olmali)";
        return false;
    }

    // Sıra önemli: kütüphaneler, onları kullanan nesne dosyasından sonra gelir.
    // -lm: sin, pow, kayan noktalı % (fmod) gibi işlemler libm'e iner.
    std::vector<llvm::StringRef> args = {*linker, objectPath, runtimeLib, "-o", exePath, "-lm"};

    // Paralel döngüler MLIR'in iş parçacığı havuzunu (libmlir_async_runtime.so)
    // kullanır. Paylaşımlı bir kütüphanedir; program çalışırken de bulunabilmesi
    // için yolu çalıştırılabilir dosyaya yazılır (rpath).
#ifdef MARLIN_LLVM_LIB_DIR
    const std::string libDir = MARLIN_LLVM_LIB_DIR;
    const std::string searchPath = "-L" + libDir;
    const std::string runPath = "-Wl,-rpath," + libDir;
    if (needsThreads || needsGpu) {
        args.push_back(searchPath);
        args.push_back(runPath);
    }
    if (needsThreads) args.push_back("-lmlir_async_runtime");
    // GPU'da çalışan kod, GPU ile MLIR'in CUDA sarmalayıcısı üzerinden konuşur
    // (bellek ayırma, kopyalama, çekirdek başlatma). O da sistemdeki NVIDIA
    // sürücüsünü (libcuda) kullanır.
    if (needsGpu) args.push_back("-lmlir_cuda_runtime");
#else
    (void)needsThreads;
    (void)needsGpu;
#endif
    int status = llvm::sys::ExecuteAndWait(*linker, args, std::nullopt, {}, 0, 0, &error);
    if (status != 0) {
        if (error.empty()) error = "baglama basarisiz (cikis kodu " + std::to_string(status) + ")";
        return false;
    }
    return true;
}

} // namespace marlin
