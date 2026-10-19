#include "NativeCodegen.h"

#include <algorithm>
#include <cstdlib>

#include "llvm/ADT/StringSet.h"
#include "llvm/BinaryFormat/MachO.h"
#include "llvm/Object/COFF.h"
#include "llvm/Object/MachO.h"
#include "llvm/Object/MachOUniversal.h"
#include "llvm/IR/LegacyPassManager.h"
#include "llvm/MC/TargetRegistry.h"
#include "llvm/Passes/PassBuilder.h"
#include "llvm/Support/CodeGen.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/MemoryBuffer.h"
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

// Derleyicinin bulunduğu dizin. (Adresi alınan sembol yalnızca çalışan
// dosyanın yerini bulmak içindir; argv0 yalnızca işletim sistemi yolu
// veremezse kullanılır.)
static std::string compilerDirectory(const char* argv0 = "") {
    static int anchor;
    return llvm::sys::path::parent_path(llvm::sys::fs::getMainExecutable(argv0, &anchor)).str();
}

std::string NativeCodegen::packageLibraryDir() {
    llvm::SmallString<256> dir(llvm::sys::path::parent_path(compilerDirectory()));
    llvm::sys::path::append(dir, "lib", "marlin");
    return llvm::sys::fs::is_directory(dir) ? std::string(dir) : "";
}

std::string NativeCodegen::findRuntimeLibrary(const char* argv0) {
    if (const char* fromEnv = std::getenv("MARLIN_RUNTIME"))
        return llvm::sys::fs::exists(fromEnv) ? fromEnv : "";

    llvm::SmallString<256> beside(compilerDirectory(argv0));
    llvm::sys::path::append(beside, runtimeLibraryName());
    if (llvm::sys::fs::exists(beside)) return std::string(beside);

    if (const std::string package = packageLibraryDir(); !package.empty()) {
        llvm::SmallString<256> path(package);
        llvm::sys::path::append(path, runtimeLibraryName());
        if (llvm::sys::fs::exists(path)) return std::string(path);
    }

#ifdef MARLIN_RUNTIME_LIB
    if (llvm::sys::fs::exists(MARLIN_RUNTIME_LIB)) return MARLIN_RUNTIME_LIB;
#endif
    return "";
}

// MACOS'TA VULKAN: MoltenVK
//
// macOS'ta Vulkan sürücüsü yoktur; MoltenVK Vulkan'ı Metal üzerinde
// gerçekleştiren bir kütüphanedir (libMoltenVK.dylib). Sisteme kurulu
// olması beklenmez: GPU kullanan programın yanına kopyalanır, çalışma
// zamanı onu orada arar (runtime/marlin_gpu_vulkan.c). Kütüphane derleme
// makinesinde şuralarda aranır: MARLIN_MOLTENVK (dosya yolu), derleyici
// paketi (lib/marlin), Vulkan SDK ($VULKAN_SDK/lib), MLIR'in kütüphane dizini
// (geliştirme derlemesi; conda ortamı: `moltenvk` paketi), Homebrew
// (`molten-vk`). Bulunamazsa kopyalanmaz; program
// sisteme kurulu Vulkan'ı kullanır, o da yoksa CPU'da çalışır.
//
// Kütüphanenin kendi yanında aradığı bağımlılıkları (@rpath/..., ör.
// conda'nın libc++'ı) da kopyalanır; sistem kütüphaneleri (/usr/lib,
// /System) her Mac'te vardır.
static std::string findMoltenVK() {
    if (const char* fromEnv = std::getenv("MARLIN_MOLTENVK"))
        return llvm::sys::fs::exists(fromEnv) ? fromEnv : "";
    std::vector<std::string> dirs;
    if (const std::string package = NativeCodegen::packageLibraryDir(); !package.empty()) dirs.push_back(package);
    if (const char* sdk = std::getenv("VULKAN_SDK")) dirs.push_back(std::string(sdk) + "/lib");
#ifdef MARLIN_LLVM_LIB_DIR
    dirs.push_back(MARLIN_LLVM_LIB_DIR);
#endif
    dirs.push_back("/opt/homebrew/lib");
    dirs.push_back("/usr/local/lib");
    for (const std::string& dir : dirs) {
        llvm::SmallString<256> path(dir);
        llvm::sys::path::append(path, "libMoltenVK.dylib");
        if (llvm::sys::fs::exists(path)) return std::string(path);
    }
    return "";
}

// Mach-O kütüphanesinin @rpath / @loader_path ile yüklediği kütüphanelerin
// dosya adları. (Evrensel ikili dosyada her mimarinin listesi birleşir.)
static std::vector<std::string> relativeDependencies(const std::string& path) {
    std::vector<std::string> names;
    llvm::Expected<llvm::object::OwningBinary<llvm::object::Binary>> binary = llvm::object::createBinary(path);
    if (!binary) {
        llvm::consumeError(binary.takeError());
        return names;
    }
    auto collect = [&](const llvm::object::MachOObjectFile& object) {
        for (const llvm::object::MachOObjectFile::LoadCommandInfo& command : object.load_commands()) {
            if (command.C.cmd != llvm::MachO::LC_LOAD_DYLIB && command.C.cmd != llvm::MachO::LC_LOAD_WEAK_DYLIB)
                continue;
            const auto dylib = object.getDylibIDLoadCommand(command);
            llvm::StringRef name(command.Ptr + dylib.dylib.name);
            if (name.starts_with("@rpath/") || name.starts_with("@loader_path/")) {
                std::string file = llvm::sys::path::filename(name).str();
                if (std::find(names.begin(), names.end(), file) == names.end()) names.push_back(file);
            }
        }
    };
    llvm::object::Binary* bin = binary->getBinary();
    if (auto* macho = llvm::dyn_cast<llvm::object::MachOObjectFile>(bin)) {
        collect(*macho);
    } else if (auto* universal = llvm::dyn_cast<llvm::object::MachOUniversalBinary>(bin)) {
        for (const auto& slice : universal->objects()) {
            auto object = slice.getAsObjectFile();
            if (object) collect(**object);
            else llvm::consumeError(object.takeError());
        }
    }
    return names;
}

static bool bundleMoltenVK(const std::string& exePath, std::string& error) {
    const std::string source = findMoltenVK();
    if (source.empty()) return true;
    llvm::SmallString<256> exeDir(exePath);
    llvm::sys::path::remove_filename(exeDir);
    if (exeDir.empty()) exeDir = ".";

    auto copyBeside = [&](const std::string& from, llvm::StringRef name) {
        llvm::SmallString<256> target(exeDir);
        llvm::sys::path::append(target, name);
        if (std::error_code ec = llvm::sys::fs::copy_file(from, target)) {
            error = from + " kopyalanamadi: " + ec.message();
            return false;
        }
        return true;
    };
    if (!copyBeside(source, "libMoltenVK.dylib")) return false;
    // (libMoltenVK.dylib genelde sürümlü dosyaya bağlantıdır; gerçek dosya okunur.)
    const llvm::StringRef sourceDir = llvm::sys::path::parent_path(source);
    for (const std::string& name : relativeDependencies(source)) {
        llvm::SmallString<256> dependency(sourceDir);
        llvm::sys::path::append(dependency, name);
        if (name.rfind("libMoltenVK", 0) == 0 || !llvm::sys::fs::exists(dependency)) continue;
        if (!copyBeside(std::string(dependency), name)) return false;
    }
    return true;
}

// WINDOWS'TA DIŞ KÜTÜPHANELER (libpq, libcurl)
//
// Program bu DLL'leri çalışırken yükler (runtime/marlin_library.c) ve
// Windows onları programın yanında ve sistem dizinlerinde arar. Sistemde
// bulunmadıkları için derleyicinin yanındaki (paketin bin/ dizinindeki)
// kopyaları programın yanına konur; DLL'in import tablosundan okunan ve aynı
// dizinde bulunan bağımlılıkları da (OpenSSL, zlib ...). Derleyicinin
// yanında olmayan bağımlılık sistemindir (kernel32 ...) ve kopyalanmaz.
static bool copyDllWithDependencies(llvm::StringRef name, const std::string& fromDir, const std::string& toDir,
                                    llvm::StringSet<>& copied, std::string& error) {
    if (!copied.insert(name.lower()).second) return true;
    llvm::SmallString<256> source(fromDir);
    llvm::sys::path::append(source, name);
    if (!llvm::sys::fs::exists(source)) return true;
    llvm::SmallString<256> target(toDir);
    llvm::sys::path::append(target, name);
    // (Program derleyicinin dizinine üretiliyorsa DLL zaten oradadır;
    // dosyayı kendi üstüne kopyalamak onu boşaltırdı.)
    if (!llvm::sys::fs::equivalent(source, target)) {
        if (std::error_code ec = llvm::sys::fs::copy_file(source, target)) {
            error = std::string(source) + " kopyalanamadi: " + ec.message();
            return false;
        }
    }
    llvm::Expected<llvm::object::OwningBinary<llvm::object::Binary>> binary =
        llvm::object::createBinary(source);
    if (!binary) {
        llvm::consumeError(binary.takeError());
        return true;
    }
    auto* coff = llvm::dyn_cast<llvm::object::COFFObjectFile>(binary->getBinary());
    if (!coff) return true;
    std::vector<std::string> dependencies;
    for (const llvm::object::ImportDirectoryEntryRef& entry : coff->import_directories()) {
        llvm::StringRef dependency;
        if (llvm::Error e = entry.getName(dependency)) llvm::consumeError(std::move(e));
        else dependencies.push_back(dependency.str());
    }
    for (const std::string& dependency : dependencies)
        if (!copyDllWithDependencies(dependency, fromDir, toDir, copied, error)) return false;
    return true;
}

// Windows: clang PATH'te değilse kurulduğu bilinen yerler. Visual Studio
// (Build Tools dahil) ve LLVM'in kurucusu clang'ı varsayılan olarak PATH'e
// eklemez. Sırayla: LLVM'in kurucusu (C:\Program Files\LLVM), Visual
// Studio'nun "C++ Clang tools" bileşeni (yerini Visual Studio'nun kendi aracı
// vswhere söyler). clang, MSVC kütüphanelerini ve Windows SDK'yı kendisi bulur.
static std::string findWindowsClang() {
    for (const char* variable : {"ProgramFiles", "ProgramW6432"}) {
        if (const char* root = std::getenv(variable)) {
            llvm::SmallString<256> path(root);
            llvm::sys::path::append(path, "LLVM", "bin", "clang.exe");
            if (llvm::sys::fs::exists(path)) return std::string(path);
        }
    }
    const char* x86 = std::getenv("ProgramFiles(x86)");
    if (!x86) return "";
    llvm::SmallString<256> vswhere(x86);
    llvm::sys::path::append(vswhere, "Microsoft Visual Studio", "Installer", "vswhere.exe");
    if (!llvm::sys::fs::exists(vswhere)) return "";
    llvm::SmallString<128> output;
    if (llvm::sys::fs::createTemporaryFile("marlin-vswhere", "txt", output)) return "";
    const std::string outputPath(output);
    const llvm::StringRef args[] = {vswhere, "-latest", "-products", "*", "-find", "VC\\Tools\\Llvm\\x64\\bin\\clang.exe"};
    const std::optional<llvm::StringRef> redirects[] = {llvm::StringRef(""), llvm::StringRef(outputPath),
                                                        llvm::StringRef("")};
    std::string found;
    if (llvm::sys::ExecuteAndWait(vswhere, args, std::nullopt, redirects, 10) == 0) {
        if (auto buffer = llvm::MemoryBuffer::getFile(outputPath)) {
            llvm::StringRef first = (*buffer)->getBuffer().split('\n').first.trim();
            if (!first.empty() && llvm::sys::fs::exists(first)) found = first.str();
        }
    }
    llvm::sys::fs::remove(outputPath);
    return found;
}

std::string NativeCodegen::findLinker(std::string& error) {
    const bool windows = hostTriple().isOSWindows();
    const std::vector<const char*> candidates =
        windows ? std::vector<const char*>{"clang"} : std::vector<const char*>{"cc", "clang", "gcc"};
    for (const char* name : candidates)
        if (llvm::ErrorOr<std::string> path = llvm::sys::findProgramByName(name)) return *path;
    if (windows)
        if (std::string clang = findWindowsClang(); !clang.empty()) return clang;
    error = windows ? "baglayici bulunamadi (clang: PATH'te, C:\\Program Files\\LLVM'de ya da Visual Studio'nun "
                      "'C++ Clang tools' bileseninde aranir)"
                    : "baglayici bulunamadi (cc, clang ya da gcc PATH'te olmali)";
    return "";
}

// Bağlayıcı olarak C derleyicisinin sürücüsü çağrılır (cc / clang / gcc): o,
// platformun sistem kütüphanelerini ve başlangıç dosyalarını kendisi ekler.
// Windows'ta clang (MSVC hedefinde lld-link ya da link.exe'yi çağırır).
bool NativeCodegen::link(const std::string& objectPath, const std::string& runtimeLib,
                         const std::string& exePath, bool needsGpu,
                         const std::vector<std::string>& sideLibraries, std::string& error) {
    const llvm::Triple triple = hostTriple();
    const bool windows = triple.isOSWindows();

    const std::string linker = findLinker(error);
    if (linker.empty()) return false;

    // Sıra önemli: kütüphaneler, onları kullanan nesne dosyasından sonra gelir.
    std::vector<std::string> args = {linker, objectPath, runtimeLib, "-o", exePath};

    if (windows) {
        llvm::SmallString<256> exeDir(exePath);
        llvm::sys::path::remove_filename(exeDir);
        if (exeDir.empty()) exeDir = ".";
        llvm::StringSet<> copied;
        for (const std::string& dll : sideLibraries)
            if (!copyDllWithDependencies(dll, compilerDirectory(), std::string(exeDir), copied, error)) return false;
    }

    // Paralel döngüler ve görevler (iş parçacığı havuzu) ile GPU katmanı
    // çalışma zamanı kütüphanesindedir (runtime/marlin_async.c,
    // runtime/marlin_gpu.c); program başka bir paylaşımlı kütüphaneye
    // bağlanmaz. GPU sürücüleri (CUDA, Vulkan) program açıkken yüklenir; böylece
    // program GPU'su olmayan makinede de açılır. macOS'ta Vulkan'ın kendisi
    // (MoltenVK) programın yanına konur.
    if (needsGpu && triple.isOSDarwin() && !bundleMoltenVK(exePath, error)) return false;

    // Sistem kütüphaneleri en sonda (statik bağlanan dış kütüphaneler de kullanabilir):
    //   POSIX: libm (sin, pow, kayan noktalı % gibi işlemler); macOS'ta libSystem'in
    //          parçasıdır, -lm yine de geçerlidir.
    //   Windows: matematik C kütüphanesindedir; soketler ws2_32, argümanlar shell32.
    if (windows) {
        args.push_back("-lws2_32");
        args.push_back("-lshell32");
        // C kütüphanesinin DLL hali (msvcrt → ucrt, vcruntime): runtime /MD ile
        // derlenir. clang sürücüsü bağlarken statik libcmt'yi ekler; ikisi
        // karışınca runtime'ın __imp_ sembolleri çözülemez. Statik olan
        // dışlanır, DLL olan açıkça bağlanır.
        args.push_back("-Wl,/nodefaultlib:libcmt");
        args.push_back("-lmsvcrt");
    } else {
        args.push_back("-lm");
        // dlopen (GPU sürücüsünü yüklemek için) ve pthread (iş parçacığı
        // havuzu): glibc 2.34'ten önce ayrı kütüphanelerdeydi. macOS'ta
        // libSystem'dedir.
        if (triple.isOSLinux()) {
            args.push_back("-ldl");
            args.push_back("-pthread");
        }
        // macOS: çalışma zamanının Objective-C parçası (platform_apple.m).
        if (triple.isOSDarwin()) args.push_back("-lobjc");
    }

    std::vector<llvm::StringRef> argRefs(args.begin(), args.end());
    int status = llvm::sys::ExecuteAndWait(linker, argRefs, std::nullopt, {}, 0, 0, &error);
    if (status != 0) {
        if (error.empty()) error = "baglama basarisiz (cikis kodu " + std::to_string(status) + ")";
        return false;
    }
    return true;
}

} // namespace marlin
