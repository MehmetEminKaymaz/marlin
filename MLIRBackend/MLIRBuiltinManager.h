#ifndef MLIRBUILTINMANAGER_H
#define MLIRBUILTINMANAGER_H

#include "CodegenContext.h"
#include "MLIRArithManager.h"
#include "MLIRRuntime.h"
#include "../AST/Builtins.h"
#include "../AST/SemType.h"
#include "llvm/ADT/ArrayRef.h"
#include "mlir/Dialect/Func/IR/FuncOps.h"

// ---------------------------------------------------------------------------
// MLIRBuiltinManager — yerleşik fonksiyonların kod üretimi
//
//   * Matematik yerleşikleri MLIR operasyonlarına çevrilir (math.sqrt,
//     math.powf, arith.maxsi ...). Hangi kütüphane fonksiyonuna ya da makine
//     komutuna ineceklerine hedefe göre indirme hattı karar verir.
//
//   * print, çalışma zamanı kütüphanesindeki tipe özel C fonksiyonlarına
//     (minik_print_i64, minik_print_str ...) çağrı olarak üretilir (bkz.
//     MLIRRuntime). İleride GPU çekirdeklerinde print'in yasaklanması ya da
//     yönlendirilmesi bu sınırda yapılır.
// ---------------------------------------------------------------------------
class MLIRBuiltinManager {
    CodegenContext& ctx;
    MLIRArithManager& arith;
    MLIRRuntime& runtime;

public:
    MLIRBuiltinManager(CodegenContext& context, MLIRArithManager& arithManager,
                       MLIRRuntime& runtimeCalls)
        : ctx(context), arith(arithManager), runtime(runtimeCalls) {}

    /// Matematik yerleşiği. Argümanlar Sema tarafından `argType` tipine
    /// getirilmiştir; sonuç da o tiptedir.
    mlir::Value buildMath(Builtin builtin, llvm::ArrayRef<mlir::Value> args,
                          const SemType& argType, mlir::Location loc);

    /// print(a, b, ...): değerleri aralarında boşlukla yazar, satır sonu ekler.
    void buildPrint(llvm::ArrayRef<mlir::Value> args, llvm::ArrayRef<SemType> types,
                    mlir::Location loc);

    /// Tek bir değeri (satır sonu eklemeden) yazar. Matris yazdırma gibi
    /// kendi düzenini kuran çağıranlar için.
    void printValue(mlir::Value value, const SemType& type, mlir::Location loc);
    void printChar(char c, mlir::Location loc);
};

#endif //MLIRBUILTINMANAGER_H
