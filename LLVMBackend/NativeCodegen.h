#ifndef NATIVECODEGEN_H
#define NATIVECODEGEN_H

#include <memory>
#include <string>
#include <vector>

#include "llvm/IR/Module.h"
#include "llvm/Target/TargetMachine.h"

// ---------------------------------------------------------------------------
// LLVM IR → makine kodu (derleyicinin çalıştığı makine için)
//
//   llvm::Module ──configure──▶ ──optimize──▶ ──emitObject──▶ .o ──link──▶ çalıştırılabilir
// ---------------------------------------------------------------------------
namespace marlin {

class NativeCodegen {
    std::unique_ptr<llvm::TargetMachine> targetMachine;
    unsigned optLevel;

    NativeCodegen(std::unique_ptr<llvm::TargetMachine> tm, unsigned level)
        : targetMachine(std::move(tm)), optLevel(level) {}

public:
    /// optLevel: 0..3 (-O0 .. -O3). Başarısızsa nullptr döner, `error` dolar.
    static std::unique_ptr<NativeCodegen> create(unsigned optLevel, std::string& error);

    /// Modüle hedef üçlüsünü ve veri yerleşimini yazar. Diğer adımlardan önce çağrılmalı.
    void configure(llvm::Module& module);

    /// LLVM'in standart hattını çalıştırır. -O0'da optimizasyon yapılmaz,
    /// yalnızca zorunlu indirmeler (coroutine'ler) uygulanır.
    void optimize(llvm::Module& module);

    /// Modülü nesne dosyası (.o) olarak yazar.
    bool emitObject(llvm::Module& module, const std::string& path, std::string& error);

    /// Nesne dosyasını sistemin C derleyicisi (Linux/macOS'ta cc, Windows'ta
    /// clang) ile bağlayıp çalıştırılabilir dosya üretir; C çalışma zamanı ve
    /// platformun sistem kütüphaneleri böylece otomatik eklenir.
    /// `runtimeLib`: marlin çalışma zamanı kütüphanesi (runtimeLibraryName()).
    /// `needsThreads`: program paralel döngü içeriyor; iş parçacığı çalışma
    /// zamanı da bağlanır.
    /// `needsGpu`: program GPU'da çalışan kod içeriyor; GPU çalışma zamanı da
    /// bağlanır.
    /// `extraLibs`: programın kullandığı standart kütüphane modüllerinin dış C
    /// kütüphaneleri (tam yol; ör. libpq).
    static bool link(const std::string& objectPath, const std::string& runtimeLib,
                     const std::string& exePath, bool needsThreads, bool needsGpu,
                     const std::vector<std::string>& extraLibs, std::string& error);

    /// Çalışma zamanı kütüphanesinin yolunu bulur: önce MARLIN_RUNTIME ortam
    /// değişkeni, sonra derleyicinin yanındaki dosya, sonra kurulu paket
    /// (packageLibraryDir), en son (geliştirme derlemesinde) derleme sırasında
    /// gömülen yol. Bulunamazsa boş döner.
    static std::string findRuntimeLibrary(const char* argv0);

    /// Derleyici kurulu bir paketten çalışıyorsa paketin kütüphane dizini:
    /// derleyici <önek>/bin içindeyse <önek>/lib/marlin (çalışma zamanı, std,
    /// programlara bağlanan paylaşımlı kütüphaneler). Yoksa boş döner.
    static std::string packageLibraryDir();

    /// Çalışma zamanı kütüphanesinin dosya adı: libmarlinrt.a (Linux, macOS,
    /// MinGW), marlinrt.lib (Windows / MSVC).
    static const char* runtimeLibraryName();

    /// -o verilmezse üretilen dosyanın adı: a.out, Windows'ta a.exe.
    static const char* defaultExecutableName();
};

} // namespace marlin

#endif //NATIVECODEGEN_H
