//
// Created by Eminf on 19.07.2026.
//

#include "MLIRArithManager.h"

using namespace mlir;

Value MLIRArithManager::buildBinary(BinaryExpr::Op op, Value left, Value right,
                                    const SemType& operandType, Location loc) {
    OpBuilder& builder = ctx.builder;
    const bool isFloat = operandType.isFloat();
    // char ve bool karşılaştırmaları işaretsiz yapılır
    const bool isUnsigned = !isFloat && !(operandType.isInt() && operandType.isSigned);

    auto compare = [&](arith::CmpFPredicate fp, arith::CmpIPredicate sp,
                       arith::CmpIPredicate up) -> Value {
        if (isFloat) return builder.create<arith::CmpFOp>(loc, fp, left, right);
        return builder.create<arith::CmpIOp>(loc, isUnsigned ? up : sp, left, right);
    };

    switch (op) {
        case BinaryExpr::Add:
            if (isFloat) return builder.create<arith::AddFOp>(loc, left, right);
            return builder.create<arith::AddIOp>(loc, left, right);
        case BinaryExpr::Sub:
            if (isFloat) return builder.create<arith::SubFOp>(loc, left, right);
            return builder.create<arith::SubIOp>(loc, left, right);
        case BinaryExpr::Mul:
            if (isFloat) return builder.create<arith::MulFOp>(loc, left, right);
            return builder.create<arith::MulIOp>(loc, left, right);
        case BinaryExpr::Div:
            if (isFloat) return builder.create<arith::DivFOp>(loc, left, right);
            if (isUnsigned) return builder.create<arith::DivUIOp>(loc, left, right);
            return builder.create<arith::DivSIOp>(loc, left, right);
        case BinaryExpr::Mod:
            if (isFloat) return builder.create<arith::RemFOp>(loc, left, right);
            if (isUnsigned) return builder.create<arith::RemUIOp>(loc, left, right);
            return builder.create<arith::RemSIOp>(loc, left, right);

        case BinaryExpr::Eq:
            return compare(arith::CmpFPredicate::OEQ, arith::CmpIPredicate::eq, arith::CmpIPredicate::eq);
        case BinaryExpr::Neq:
            return compare(arith::CmpFPredicate::ONE, arith::CmpIPredicate::ne, arith::CmpIPredicate::ne);
        case BinaryExpr::Lt:
            return compare(arith::CmpFPredicate::OLT, arith::CmpIPredicate::slt, arith::CmpIPredicate::ult);
        case BinaryExpr::Lte:
            return compare(arith::CmpFPredicate::OLE, arith::CmpIPredicate::sle, arith::CmpIPredicate::ule);
        case BinaryExpr::Gt:
            return compare(arith::CmpFPredicate::OGT, arith::CmpIPredicate::sgt, arith::CmpIPredicate::ugt);
        case BinaryExpr::Gte:
            return compare(arith::CmpFPredicate::OGE, arith::CmpIPredicate::sge, arith::CmpIPredicate::uge);

        // Kısa devre gerektirmeyen (yan etkisiz) kullanım için
        case BinaryExpr::And:
            return builder.create<arith::AndIOp>(loc, left, right);
        case BinaryExpr::Or:
            return builder.create<arith::OrIOp>(loc, left, right);
    }
    return nullptr;
}

Value MLIRArithManager::buildUnary(UnaryExpr::Op op, Value operand,
                                   const SemType& operandType, Location loc) {
    OpBuilder& builder = ctx.builder;

    switch (op) {
        case UnaryExpr::Negate: {
            if (operandType.isFloat()) return builder.create<arith::NegFOp>(loc, operand);
            Value zero = buildIntConstant(0, operand.getType(), loc);
            return builder.create<arith::SubIOp>(loc, zero, operand);
        }
        case UnaryExpr::Not: {
            Value trueVal = buildIntConstant(1, operand.getType(), loc);
            return builder.create<arith::XOrIOp>(loc, operand, trueVal);
        }
    }
    return nullptr;
}

Value MLIRArithManager::buildCast(Value value, const SemType& from, const SemType& to,
                                  Location loc) {
    OpBuilder& builder = ctx.builder;
    if (from == to) return value;

    // (AST'deki ::Type ile karışmaması için mlir::Type açıkça yazılıyor)
    mlir::Type target = builder.getIntegerType(to.width);
    if (to.isFloat())
        target = to.width == 32 ? mlir::Type(builder.getF32Type()) : mlir::Type(builder.getF64Type());

    if (from.isInt() && to.isInt()) {
        if (from.width == to.width) return value; // yalnızca işaret yorumu değişiyor
        if (from.width > to.width) return builder.create<arith::TruncIOp>(loc, target, value);
        if (from.isSigned) return builder.create<arith::ExtSIOp>(loc, target, value);
        return builder.create<arith::ExtUIOp>(loc, target, value);
    }
    if (from.isInt() && to.isFloat()) {
        if (from.isSigned) return builder.create<arith::SIToFPOp>(loc, target, value);
        return builder.create<arith::UIToFPOp>(loc, target, value);
    }
    if (from.isFloat() && to.isInt()) {
        if (to.isSigned) return builder.create<arith::FPToSIOp>(loc, target, value);
        return builder.create<arith::FPToUIOp>(loc, target, value);
    }
    if (from.isFloat() && to.isFloat()) {
        if (from.width < to.width) return builder.create<arith::ExtFOp>(loc, target, value);
        return builder.create<arith::TruncFOp>(loc, target, value);
    }
    return value;
}

Value MLIRArithManager::buildIntConstant(int64_t value, mlir::Type mlirType, Location loc) {
    OpBuilder& builder = ctx.builder;
    if (mlirType.isIndex())
        return builder.create<arith::ConstantIndexOp>(loc, value);

    unsigned width = mlirType.getIntOrFloatBitWidth();
    llvm::APInt bits = llvm::APInt(64, static_cast<uint64_t>(value)).zextOrTrunc(width);
    return builder.create<arith::ConstantOp>(loc, builder.getIntegerAttr(mlirType, bits));
}

Value MLIRArithManager::buildFloatConstant(double value, FloatType mlirType, Location loc) {
    bool lossy = false;
    llvm::APFloat apValue(value);
    apValue.convert(mlirType.getFloatSemantics(), llvm::APFloat::rmNearestTiesToEven, &lossy);
    return ctx.builder.create<arith::ConstantFloatOp>(loc, apValue, mlirType);
}
