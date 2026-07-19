//
// Created by Eminf on 19.07.2026.
//

#ifndef MLIRARITHMANAGER_H
#define MLIRARITHMANAGER_H

#include "CodegenContext.h"
#include "../AST/ASTNode.h"
#include "mlir/Dialect/Arith/IR/Arith.h"

class MLIRArithManager {
    CodegenContext& ctx;
public:
    MLIRArithManager(CodegenContext& context) : ctx(context) {}

    mlir::Value buildBinary(BinaryExpr::Op op, mlir::Value left, mlir::Value right, mlir::Location loc) {
        mlir::OpBuilder& builder = ctx.builder;
        bool isFloat = mlir::isa<mlir::FloatType>(left.getType());

        switch (op) {
            case BinaryExpr::Add:
                if (isFloat) return builder.create<mlir::arith::AddFOp>(loc, left, right);
                else return builder.create<mlir::arith::AddIOp>(loc, left, right);
            case BinaryExpr::Sub:
                if (isFloat) return builder.create<mlir::arith::SubFOp>(loc, left, right);
                else return builder.create<mlir::arith::SubIOp>(loc, left, right);
            case BinaryExpr::Mul:
                if (isFloat) return builder.create<mlir::arith::MulFOp>(loc, left, right);
                else return builder.create<mlir::arith::MulIOp>(loc, left, right);
            case BinaryExpr::Div:
                if (isFloat) return builder.create<mlir::arith::DivFOp>(loc, left, right);
                else return builder.create<mlir::arith::DivSIOp>(loc, left, right);
            case BinaryExpr::Mod:
                if (isFloat) return builder.create<mlir::arith::RemFOp>(loc, left, right);
                else return builder.create<mlir::arith::RemSIOp>(loc, left, right);
            case BinaryExpr::Eq:
                if (isFloat) return builder.create<mlir::arith::CmpFOp>(loc, mlir::arith::CmpFPredicate::OEQ, left, right);
                else return builder.create<mlir::arith::CmpIOp>(loc, mlir::arith::CmpIPredicate::eq, left, right);
            case BinaryExpr::Neq:
                if (isFloat) return builder.create<mlir::arith::CmpFOp>(loc, mlir::arith::CmpFPredicate::ONE, left, right);
                else return builder.create<mlir::arith::CmpIOp>(loc, mlir::arith::CmpIPredicate::ne, left, right);
            case BinaryExpr::Lt:
                if (isFloat) return builder.create<mlir::arith::CmpFOp>(loc, mlir::arith::CmpFPredicate::OLT, left, right);
                else return builder.create<mlir::arith::CmpIOp>(loc, mlir::arith::CmpIPredicate::slt, left, right);
            case BinaryExpr::Lte:
                if (isFloat) return builder.create<mlir::arith::CmpFOp>(loc, mlir::arith::CmpFPredicate::OLE, left, right);
                else return builder.create<mlir::arith::CmpIOp>(loc, mlir::arith::CmpIPredicate::sle, left, right);
            case BinaryExpr::Gt:
                if (isFloat) return builder.create<mlir::arith::CmpFOp>(loc, mlir::arith::CmpFPredicate::OGT, left, right);
                else return builder.create<mlir::arith::CmpIOp>(loc, mlir::arith::CmpIPredicate::sgt, left, right);
            case BinaryExpr::Gte:
                if (isFloat) return builder.create<mlir::arith::CmpFOp>(loc, mlir::arith::CmpFPredicate::OGE, left, right);
                else return builder.create<mlir::arith::CmpIOp>(loc, mlir::arith::CmpIPredicate::sge, left, right);
            case BinaryExpr::And:
                return builder.create<mlir::arith::AndIOp>(loc, left, right);
            case BinaryExpr::Or:
                return builder.create<mlir::arith::OrIOp>(loc, left, right);
        }
        return nullptr;
    }

    mlir::Value buildUnary(UnaryExpr::Op op, mlir::Value operand, mlir::Location loc) {
        mlir::OpBuilder& builder = ctx.builder;
        bool isFloat = mlir::isa<mlir::FloatType>(operand.getType());

        switch (op) {
            case UnaryExpr::Negate: {
                if (isFloat) return builder.create<mlir::arith::NegFOp>(loc, operand);
                else {
                    auto zero = builder.create<mlir::arith::ConstantOp>(loc, builder.getIntegerAttr(operand.getType(), 0));
                    return builder.create<mlir::arith::SubIOp>(loc, zero, operand);
                }
            }
            case UnaryExpr::Not: {
                auto trueVal = builder.create<mlir::arith::ConstantIntOp>(loc, 1, 1);
                return builder.create<mlir::arith::XOrIOp>(loc, operand, trueVal);
            }
        }
        return nullptr;
    }
};



#endif //MLIRARITHMANAGER_H
