#ifndef LLVMLOWERING_H
#define LLVMLOWERING_H

#include <memory>
#include <string>

#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "mlir/IR/BuiltinOps.h"
#include "mlir/IR/DialectRegistry.h"

// ---------------------------------------------------------------------------
// MLIR → LLVM IR indirme hattı
//
//   üst seviye MLIR (linalg, tensor, scf, cf, arith, math, memref, func, llvm)
//        │  lowerToLLVMDialect      — dialect dönüşüm pass'leri
//        ▼
//   yalnızca llvm dialect'i
//        │  translateToLLVMIR       — MLIR'den çıkış
//        ▼
//   llvm::Module
//
// Sonrası (optimizasyon, nesne dosyası, bağlama) NativeCodegen'dedir.
// ---------------------------------------------------------------------------
namespace marlin {

/// İndirme hattının ihtiyaç duyduğu arayüzleri registry'ye ekler. MLIRContext
/// oluşturulmadan önce çağrılmalıdır.
void registerLoweringInterfaces(mlir::DialectRegistry& registry);

/// GPU çekirdeklerini derleyecek araçlar (NVIDIA: CUDA araç takımının
/// libdevice'ı). Derleyicinin çalıştığı makinede aranır: CUDA_ROOT /
/// CUDA_HOME / CUDA_PATH ya da MLIR derlenirken bulunan yol.
struct GpuToolchain {
    bool available = false;
    std::string toolkit;   // CUDA araç takımının dizini
    std::string format;    // gömülen çekirdeğin biçimi: "isa" (PTX; sürücü derler)
    std::string missing;   // bulunamadıysa sebebi
};
GpuToolchain findGpuToolchain();

/// Modüldeki bütün dialect'leri llvm dialect'ine indirir. Sıra önemlidir:
/// önce yapısal akış (scf) bloklara açılır, sonra kalan her şey çevrilir.
/// Hata durumunda false döner (ayrıntı MLIR tanılama çıktısındadır).
/// `gpu`: modülde GPU için üretilmiş fonksiyon varsa GPU adımları eklenir;
/// çekirdekler bu araçlarla derlenir (yoksa nullptr).
/// `stopAfterGpuSplit`: GPU çekirdekleri ayrıldıktan sonra dur (incelemek için).
bool lowerToLLVMDialect(mlir::ModuleOp module, const GpuToolchain* gpu, bool stopAfterGpuSplit = false);

/// llvm dialect'indeki modülü LLVM IR'a çevirir ve programın giriş noktasını
/// C çalışma zamanının beklediği `i32 main()` biçimine getirir. Başarısızsa
/// nullptr döner.
/// `usesTasks`: program görev başlatıyor; main bitince süreç hâlâ çalışan
/// görevleri beklemeden kapanır (marlin_rt_exit).
std::unique_ptr<llvm::Module> translateToLLVMIR(mlir::ModuleOp module,
                                                llvm::LLVMContext& llvmContext,
                                                bool usesTasks = false);

} // namespace marlin

#endif //LLVMLOWERING_H
