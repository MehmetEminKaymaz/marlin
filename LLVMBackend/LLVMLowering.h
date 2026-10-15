#ifndef LLVMLOWERING_H
#define LLVMLOWERING_H

#include <memory>

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

/// Modüldeki bütün dialect'leri llvm dialect'ine indirir. Sıra önemlidir:
/// önce yapısal akış (scf) bloklara açılır, sonra kalan her şey çevrilir.
/// Hata durumunda false döner (ayrıntı MLIR tanılama çıktısındadır).
/// `usesGpu`: modülde GPU için üretilmiş fonksiyon var; GPU adımları eklenir.
/// `stopAfterGpuSplit`: GPU çekirdekleri ayrıldıktan sonra dur (incelemek için).
bool lowerToLLVMDialect(mlir::ModuleOp module, bool usesGpu, bool stopAfterGpuSplit = false);

/// llvm dialect'indeki modülü LLVM IR'a çevirir ve programın giriş noktasını
/// C çalışma zamanının beklediği `i32 main()` biçimine getirir. Başarısızsa
/// nullptr döner.
std::unique_ptr<llvm::Module> translateToLLVMIR(mlir::ModuleOp module,
                                                llvm::LLVMContext& llvmContext);

} // namespace marlin

#endif //LLVMLOWERING_H
