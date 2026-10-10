//
// Created by Eminf on 19.07.2026.
//

#ifndef MLIRARITHMANAGER_H
#define MLIRARITHMANAGER_H

#include "CodegenContext.h"
#include "../AST/ASTNode.h"
#include "mlir/Dialect/Arith/IR/Arith.h"

// Aritmetik, karşılaştırma, sabit ve tip dönüşümü operasyonlarını üretir.
//
// MLIR tamsayıları işaretsiz olduğundan (i32 hem int hem uint'tir) doğru
// işlemi (divsi/divui, slt/ult, extsi/extui ...) seçmek için işlenenlerin
// SemType'ı da verilir. Sema, bir ikili işlemin iki tarafını aynı tipe
// getirdiği için tek bir işlenen tipi yeterlidir.
class MLIRArithManager {
    CodegenContext& ctx;
public:
    MLIRArithManager(CodegenContext& context) : ctx(context) {}

    /// Kısa devreli && ve || burada üretilmez (bkz. MLIRControlFlowManager).
    mlir::Value buildBinary(BinaryExpr::Op op, mlir::Value left, mlir::Value right,
                            const SemType& operandType, mlir::Location loc);

    mlir::Value buildUnary(UnaryExpr::Op op, mlir::Value operand,
                           const SemType& operandType, mlir::Location loc);

    /// Sayısal tipler arasında dönüşüm (genişletme, daraltma, int <-> float).
    mlir::Value buildCast(mlir::Value value, const SemType& from, const SemType& to,
                          mlir::Location loc);

    /// `mlirType` tipinde tamsayı sabiti (iN ya da index).
    mlir::Value buildIntConstant(int64_t value, mlir::Type mlirType, mlir::Location loc);
    mlir::Value buildFloatConstant(double value, mlir::FloatType mlirType, mlir::Location loc);
};



#endif //MLIRARITHMANAGER_H
