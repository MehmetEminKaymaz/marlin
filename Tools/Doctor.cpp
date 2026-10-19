// marlin doctor — kurulumun denetimi (bkz. Doctor.h).
//
// Denetimler sırayla:
//   1. paket       std dizini ve çalışma zamanı kütüphanesi bulunuyor mu
//   2. bağlayıcı   C derleyicisi sürücüsü var mı; küçük bir program (paralel
//                  döngüyle) derlenip çalışıyor mu
//   3. GPU         derlerken: NVIDIA çekirdekleri için CUDA araç takımı;
//                  çalışırken: bir @GPU programın seçtiği arka uç (cuda,
//                  vulkan ya da sebebiyle cpu)
//   4. std.http.client / std.postgres
//                  libcurl / libpq çalışırken yüklenebiliyor mu
//
// Test programları derleyicinin kendisiyle (ayrı bir süreç olarak) derlenir
// ve çalıştırılır: denetlenen, kullanıcının programlarının gerçekten geçtiği
// yoldur. Hiçbir şey kurulmaz ya da değiştirilmez; geçici dizin sonda silinir.

#include "Doctor.h"

#include "../AST/ModuleLoader.h"
#include "../LLVMBackend/LLVMLowering.h"
#include "../LLVMBackend/NativeCodegen.h"

#include "llvm/ADT/SmallString.h"
#include "llvm/Config/llvm-config.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/Program.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/TargetParser/Host.h"
#include "llvm/TargetParser/Triple.h"

#include <cstdlib>
#include <iostream>
#include <optional>
#include <string>
#include <vector>

namespace marlin {
namespace {

// ---------------------------------------------------------------------------
// Rapor
// ---------------------------------------------------------------------------

enum class Status { Ok, Info, Warning, Error };

int errorCount = 0;

void report(Status status, const std::string& what, const std::string& detail, const std::string& hint = "") {
    static const char* const labels[] = {"[tamam]", "[bilgi]", "[uyari]", "[HATA] "};
    if (status == Status::Error) ++errorCount;
    std::cout << "  " << labels[static_cast<int>(status)] << " " << what;
    if (!detail.empty()) std::cout << ": " << detail;
    std::cout << "\n";
    if (!hint.empty()) std::cout << "          -> " << hint << "\n";
}

// İlk `lines` satır, girintili (derleyicinin / programın hata çıktısı için).
std::string firstLines(const std::string& text, int lines) {
    std::string result;
    size_t start = 0;
    for (int i = 0; i < lines && start < text.size(); ++i) {
        size_t end = text.find('\n', start);
        if (end == std::string::npos) end = text.size();
        std::string line = text.substr(start, end - start);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (!line.empty()) result += "\n          " + line;
        start = end + 1;
    }
    return result;
}

std::string trimmed(std::string text) {
    while (!text.empty() && (text.back() == '\n' || text.back() == '\r' || text.back() == ' ')) text.pop_back();
    return text;
}

// ---------------------------------------------------------------------------
// Platform
// ---------------------------------------------------------------------------

const llvm::Triple& host() {
    static const llvm::Triple triple(llvm::sys::getDefaultTargetTriple());
    return triple;
}

// C derleyicisi (bağlayıcı) için kurulum önerisi.
std::string linkerHint() {
    if (host().isOSWindows())
        return "Visual Studio Build Tools'u C++ ve Clang bilesenleriyle kurun: winget install "
               "Microsoft.VisualStudio.2022.BuildTools --override \"--passive --add "
               "Microsoft.VisualStudio.Workload.VCTools --add Microsoft.VisualStudio.Component.VC.Llvm.Clang "
               "--includeRecommended\"";
    if (host().isOSDarwin()) return "Xcode Command Line Tools'u kurun: xcode-select --install";
    return "C derleyicisi kurun: Debian/Ubuntu 'sudo apt install build-essential', "
           "Fedora 'sudo dnf install gcc', Arch 'sudo pacman -S base-devel'";
}

void setEnvironment(const char* name, const char* value) {
#ifdef _WIN32
    _putenv_s(name, value);
#else
    setenv(name, value, 1);
#endif
}

// ---------------------------------------------------------------------------
// Test programları
// ---------------------------------------------------------------------------

struct Workspace {
    llvm::SmallString<256> dir;
    std::string compiler;

    std::string path(llvm::StringRef name) const {
        llvm::SmallString<256> p(dir);
        llvm::sys::path::append(p, name);
        return std::string(p);
    }
};

std::string readFile(const std::string& path) {
    auto buffer = llvm::MemoryBuffer::getFile(path);
    return buffer ? (*buffer)->getBuffer().str() : "";
}

struct RunResult {
    int code = -1;
    std::string out, err;
};

// Programı çalıştırır; çıktıları dosyalara yönlendirilir. (Süre sınırı:
// takılan bir test programı denetimi kilitlemesin.)
RunResult run(const Workspace& ws, const std::string& program, const std::vector<std::string>& args,
              unsigned timeoutSeconds) {
    const std::string out = ws.path("cikti.txt"), err = ws.path("hata.txt");
    // (Yönlendirilen dosya POSIX'te kesilmeden açılır: önceki çalışmanın
    // fazlası kalmasın.)
    llvm::sys::fs::remove(out);
    llvm::sys::fs::remove(err);
    std::vector<llvm::StringRef> argRefs = {program};
    for (const std::string& a : args) argRefs.push_back(a);
    const std::optional<llvm::StringRef> redirects[] = {llvm::StringRef(""), llvm::StringRef(out),
                                                        llvm::StringRef(err)};
    RunResult result;
    std::string message;
    result.code = llvm::sys::ExecuteAndWait(program, argRefs, std::nullopt, redirects, timeoutSeconds, 0, &message);
    result.out = readFile(out);
    result.err = readFile(err);
    if (!message.empty()) result.err += (result.err.empty() ? "" : "\n") + message;
    return result;
}

// Kaynağı yazar ve derler. Başarılıysa programın yolu; değilse boş ve
// derleyicinin hata çıktısı `errors`ta.
std::string compile(const Workspace& ws, llvm::StringRef name, llvm::StringRef source, std::string& errors) {
    const std::string sourcePath = ws.path((name + ".marl").str());
    {
        std::error_code ec;
        llvm::raw_fd_ostream file(sourcePath, ec, llvm::sys::fs::OF_Text);
        if (ec) {
            errors = "gecici dosya yazilamadi: " + ec.message();
            return "";
        }
        file << source;
    }
    const std::string exe = ws.path((name + (host().isOSWindows() ? ".exe" : "")).str());
    RunResult r = run(ws, ws.compiler, {sourcePath, "-o", exe}, 120);
    if (r.code != 0 || !llvm::sys::fs::exists(exe)) {
        errors = trimmed(r.err.empty() ? r.out : r.err);
        return "";
    }
    return exe;
}

const char* const kBasicProgram = R"(
func main() -> void {
    int[4] kareler = [0, 0, 0, 0];
    parallel for i in 0 .. 4 {
        kareler[i] = i * i;
    }
    int toplam = 0;
    for k in kareler {
        toplam = toplam + k;
    }
    print("merhaba", toplam);
}
)";

const char* const kGpuProgram = R"(
@GPU
func ikiyle(int n) -> matrix<int, 1> {
    matrix<int, 1> m = zeros(n);
    parallel for i in 0 .. n {
        m[i] = i * 2;
    }
    return m;
}

func main() -> void {
    print(sum(ikiyle(1000)));
}
)";

// Bağlantı reddedilen bir adres: kütüphane yüklenebiliyorsa istek ağ
// hatasıyla, yüklenemiyorsa kütüphane hatasıyla döner.
const char* const kCurlProgram = R"(
import std.http.client as client;

func main() -> void {
    client.Reply r = client.get("http://127.0.0.1:1/");
    print(r.error);
    free(r.headers);
}
)";

const char* const kPostgresProgram = R"(
import std.postgres as pg;

func main() -> void {
    pg.Conn c = pg.connect("host=127.0.0.1 port=1 connect_timeout=2");
    print(c.error);
}
)";

// marlin_rt_load_library'nin hataları (runtime/marlin_library.c).
bool isLibraryError(const std::string& message) {
    return message.find("bulunamadi") != std::string::npos || message.find("acilamadi") != std::string::npos ||
           message.find("cok eski ya da bozuk") != std::string::npos;
}

// ---------------------------------------------------------------------------
// Denetimler
// ---------------------------------------------------------------------------

void checkPackage(const std::string& compilerPath, const char* argv0) {
    const std::string package = NativeCodegen::packageLibraryDir();
    const std::string stdDir = ModuleLoader::findStdDir(compilerPath);
    const std::string runtime = NativeCodegen::findRuntimeLibrary(argv0);
    report(Status::Info, "derleyici", compilerPath);
    if (stdDir.empty())
        report(Status::Error, "standart kutuphane (std)", "bulunamadi",
               "paket bozuk olabilir: yeniden indirip acin (lib/marlin/std olmali) ya da MARLIN_STD ile yolunu verin");
    else
        report(Status::Ok, "standart kutuphane (std)", stdDir);
    if (runtime.empty())
        report(Status::Error, "calisma zamani", std::string(NativeCodegen::runtimeLibraryName()) + " bulunamadi",
               "paket bozuk olabilir: yeniden indirip acin ya da MARLIN_RUNTIME ile yolunu verin");
    else
        report(Status::Ok, "calisma zamani", runtime);
    if (package.empty())
        report(Status::Info, "paket dizini", "yok (derleyici gelistirme dizininden calisiyor)");
}

// Doğruysa bağlama çalışıyor: sonraki denetimler program derleyebilir.
bool checkLinker(const Workspace& ws) {
    std::string error;
    const std::string linker = NativeCodegen::findLinker(error);
    if (linker.empty()) {
        report(Status::Error, "baglayici (C derleyicisi)", error, linkerHint());
        return false;
    }
    report(Status::Ok, "baglayici (C derleyicisi)", linker);

    std::string errors;
    const std::string exe = compile(ws, "temel", kBasicProgram, errors);
    if (exe.empty()) {
        report(Status::Error, "deneme programi derlenemedi", firstLines(errors, 6), linkerHint());
        return false;
    }
    RunResult r = run(ws, exe, {}, 30);
    if (r.code != 0 || trimmed(r.out) != "merhaba 14") {
        report(Status::Error, "deneme programi calismadi",
               "cikis kodu " + std::to_string(r.code) + firstLines(r.err + r.out, 6));
        return false;
    }
    report(Status::Ok, "deneme programi", "derlendi ve calisti (paralel dongu dahil)");
    return true;
}

void checkGpu(const Workspace& ws) {
    const GpuToolchain toolchain = findGpuToolchain();
    if (toolchain.cuda)
        report(Status::Ok, "GPU derleme (NVIDIA)", "CUDA arac takimi " + toolchain.toolkit);
    else
        report(Status::Info, "GPU derleme (NVIDIA)", toolchain.missing,
               "NVIDIA kartlarda @GPU kodu Vulkan ile calisir; CUDA cekirdekleri icin CUDA Toolkit kurulabilir "
               "(istege bagli)");

    std::string errors;
    const std::string exe = compile(ws, "gpu", kGpuProgram, errors);
    if (exe.empty()) {
        report(Status::Error, "@GPU deneme programi derlenemedi", firstLines(errors, 6));
        return;
    }
    // (Program, seçtiği arka ucu stderr'e yazar: bkz. runtime/marlin_gpu.c.)
    setEnvironment("MARLIN_GPU_REPORT", "1");
    RunResult r = run(ws, exe, {}, 60);
    setEnvironment("MARLIN_GPU_REPORT", "0");
    if (r.code != 0 || trimmed(r.out) != "999000") {
        report(Status::Error, "@GPU deneme programi yanlis calisti",
               "cikis kodu " + std::to_string(r.code) + firstLines(r.err + r.out, 6));
        return;
    }
    std::string choice;
    const std::string marker = "marlin gpu: ";
    if (size_t at = r.err.find(marker); at != std::string::npos)
        choice = trimmed(r.err.substr(at + marker.size(), r.err.find('\n', at) - at - marker.size()));
    if (choice.rfind("cuda", 0) == 0 || choice.rfind("vulkan", 0) == 0) {
        report(Status::Ok, "GPU calisirken", choice);
    } else if (choice.rfind("cpu (", 0) == 0) {
        report(Status::Warning, "GPU calisirken", choice + "; @GPU fonksiyonlar CPU'da calisir");
    } else {
        std::string hint = "@GPU fonksiyonlar CPU'da calisir (dogru ama yavas). ";
        if (host().isOSDarwin())
            hint += "macOS'ta GPU, programin yanina konan MoltenVK ile Metal uzerinden kullanilir";
        else if (host().isOSWindows())
            hint += "Ekran karti surucusunu guncelleyin (NVIDIA, AMD ve Intel suruculeri Vulkan icerir)";
        else
            hint += "Ekran karti surucusunu ve Vulkan'i kurun: Debian/Ubuntu 'sudo apt install libvulkan1 "
                    "mesa-vulkan-drivers' (NVIDIA: tescilli surucu)";
        report(Status::Warning, "GPU calisirken", "kullanilabilir GPU bulunamadi", hint);
    }
}

void checkLibrary(const Workspace& ws, const char* module, const char* name, const char* source) {
    std::string errors;
    const std::string exe = compile(ws, name, source, errors);
    if (exe.empty()) {
        // (Derleyici kütüphanenin başlıkları olmadan derlenmişse modül hiç yoktur.)
        report(Status::Warning, module, "kullanilamiyor" + firstLines(errors, 3));
        return;
    }
    RunResult r = run(ws, exe, {}, 30);
    const std::string message = trimmed(r.out);
    if (r.code != 0)
        report(Status::Warning, module, "deneme programi calismadi (cikis kodu " + std::to_string(r.code) + ")" +
                                            firstLines(r.err, 4));
    else if (isLibraryError(message))
        report(Status::Warning, module, message);
    else
        report(Status::Ok, module, "kutuphane yuklenebiliyor");
}

} // namespace

int runDoctor(const char* argv0) {
    static int anchor;
    Workspace ws;
    ws.compiler = llvm::sys::fs::getMainExecutable(argv0, &anchor);

    std::cout << "marlin " << MARLIN_VERSION << " (LLVM " << LLVM_VERSION_STRING << ", " << host().str()
              << ") kurulum denetimi\n\n";
    checkPackage(ws.compiler, argv0);

    if (std::error_code ec = llvm::sys::fs::createUniqueDirectory("marlin-doctor", ws.dir)) {
        report(Status::Error, "gecici dizin olusturulamadi", ec.message());
        return 1;
    }
    if (errorCount == 0 && checkLinker(ws)) {
        checkGpu(ws);
        checkLibrary(ws, "std.http.client (libcurl)", "curl", kCurlProgram);
        checkLibrary(ws, "std.postgres (libpq)", "postgres", kPostgresProgram);
    }
    llvm::sys::fs::remove_directories(ws.dir);

    std::cout << "\n"
              << (errorCount ? "Sorunlar var: yukaridaki [HATA] satirlari duzeltilmeden program derlenemez.\n"
                             : "Kurulum calisiyor. [uyari] satirlari yalnizca ilgili ozellikleri etkiler.\n");
    return errorCount ? 1 : 0;
}

} // namespace marlin
