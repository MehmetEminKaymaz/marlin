#ifndef NATIVECODEGEN_H
#define NATIVECODEGEN_H

#include <memory>
#include <string>

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

    /// Nesne dosyasını sistemin C derleyicisi (cc) ile bağlayıp çalıştırılabilir
    /// dosya üretir; C çalışma zamanı ve libm böylece otomatik eklenir.
    /// `runtimeLib`: marlin çalışma zamanı kütüphanesi (libmarlinrt.a).
    /// `needsThreads`: program paralel döngü içeriyor; iş parçacığı çalışma
    /// zamanı da bağlanır.
    /// `needsGpu`: program GPU'da çalışan kod içeriyor; GPU çalışma zamanı da
    /// bağlanır.
    static bool link(const std::string& objectPath, const std::string& runtimeLib,
                     const std::string& exePath, bool needsThreads, bool needsGpu,
                     std::string& error);

    /// libmarlinrt.a'nın yolunu bulur: önce MARLIN_RUNTIME ortam değişkeni, sonra
    /// derleyicinin yanındaki dosya, en son derleme sırasında gömülen yol.
    /// Bulunamazsa boş döner.
    static std::string findRuntimeLibrary(const char* argv0);
};

} // namespace marlin

#endif //NATIVECODEGEN_H
