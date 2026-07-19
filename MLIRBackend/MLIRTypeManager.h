//
// Created by Eminf on 19.07.2026.
//

#ifndef MLIRTYPEMANAGER_H
#define MLIRTYPEMANAGER_H

#include "CodegenContext.h"
#include "../AST/ASTNode.h"
#include "mlir/IR/Types.h"
#include "mlir/Dialect/LLVMIR/LLVMDialect.h"
#include "mlir/Dialect/LLVMIR/LLVMTypes.h"

class MLIRTypeManager {
    CodegenContext& ctx;
public:
    MLIRTypeManager(CodegenContext& context) : ctx(context) {}

    mlir::Type convertType(Type* astType) {
        if (!astType) return nullptr;
        mlir::OpBuilder& builder = ctx.builder;

        switch (astType->kind) {
            case TypeKind::Int8: return builder.getIntegerType(8);
            case TypeKind::Int16: return builder.getIntegerType(16);
            case TypeKind::Int32: return builder.getI32Type();
            case TypeKind::Int64: return builder.getI64Type();
            case TypeKind::UInt8: return builder.getIntegerType(8);
            case TypeKind::UInt16: return builder.getIntegerType(16);
            case TypeKind::UInt32: return builder.getI32Type();
            case TypeKind::UInt64: return builder.getI64Type();
            case TypeKind::Float32: return builder.getF32Type();
            case TypeKind::Float64: return builder.getF64Type();
            case TypeKind::Bool: return builder.getI1Type();
            case TypeKind::Char: return builder.getIntegerType(8);
            case TypeKind::String: {
                auto ptrType = mlir::LLVM::LLVMPointerType::get(&ctx.context);
                auto i64Type = builder.getI64Type();
                return mlir::LLVM::LLVMStructType::getLiteral(&ctx.context, {ptrType, i64Type});
            }
            case TypeKind::Void: return builder.getNoneType(); // Or handle in function signature
            default:
                return builder.getI32Type();
        }
    }
};



#endif //MLIRTYPEMANAGER_H
