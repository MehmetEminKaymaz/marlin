//
// Created by Eminf on 19.07.2026.
//

#ifndef MLIRGENERATOR_H
#define MLIRGENERATOR_H

#include "../AST/ASTNode.h"
#include "CodegenContext.h"
#include "MLIRTypeManager.h"
#include "MLIRScopeManager.h"
#include "MLIRArithManager.h"
#include "MLIRControlFlowManager.h"
#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/SmallVector.h"
#include "mlir/Dialect/Func/IR/FuncOps.h"
#include "mlir/Dialect/MemRef/IR/MemRef.h"
#include "mlir/Dialect/LLVMIR/LLVMDialect.h"

class MLIRGenerator : public ASTVisitor {
    CodegenContext ctx;
    MLIRTypeManager typeManager;
    MLIRScopeManager scopeManager;
    MLIRArithManager arithManager;
    MLIRControlFlowManager cfManager;

    mlir::Value lastValue;
    int stringLiteralCount = 0;

public:
    MLIRGenerator(mlir::MLIRContext& context, mlir::OpBuilder& builder, mlir::ModuleOp moduleOp)
        : ctx(builder, context, moduleOp),
          typeManager(ctx), scopeManager(ctx),
          arithManager(ctx), cfManager(ctx) {}

    void visit(Program* node) override {
        // FuncOps must be inserted at module level; set builder there
        ctx.builder.setInsertionPointToEnd(ctx.theModule.getBody());
        for (auto& decl : node->declarations) {
            decl->accept(*this);
        }
    }

    void visit(Type* node) override {}

    void visit(IntegerLiteral* node) override {
        auto loc = ctx.getLoc(node->line, node->column);
        lastValue = ctx.builder.create<mlir::arith::ConstantIntOp>(loc, node->value, 32);
    }

    void visit(FloatLiteral* node) override {
        auto loc = ctx.getLoc(node->line, node->column);
        lastValue = ctx.builder.create<mlir::arith::ConstantFloatOp>(
            loc,
            llvm::APFloat(node->value),
            ctx.builder.getF64Type() // Cast işlemi silindi!
        );
    }

    void visit(BooleanLiteral* node) override {
        auto loc = ctx.getLoc(node->line, node->column);
        lastValue = ctx.builder.create<mlir::arith::ConstantIntOp>(loc, node->value ? 1 : 0, 1);
    }

    void visit(CharLiteral* node) override {
        auto loc = ctx.getLoc(node->line, node->column);
        lastValue = ctx.builder.create<mlir::arith::ConstantIntOp>(loc, node->value, 8);
    }

    void visit(StringLiteral* node) override {
        auto loc = ctx.getLoc(node->line, node->column);
        std::string globalName = "str_literal_" + std::to_string(stringLiteralCount++);

        // 1) Insert global into module body using a guard
        {
            mlir::OpBuilder::InsertionGuard guard(ctx.builder);
            ctx.builder.setInsertionPointToStart(ctx.theModule.getBody());

            auto strType = mlir::LLVM::LLVMArrayType::get(
                ctx.builder.getIntegerType(8),
                static_cast<unsigned>(node->value.size() + 1));

            ctx.builder.create<mlir::LLVM::GlobalOp>(
                loc, strType, /*isConst=*/true,
                mlir::LLVM::Linkage::Private,
                globalName,
                ctx.builder.getStringAttr(node->value + '\0'));
        }

        // 2) Get address of the global and build the fat-pointer struct
        auto ptrType = mlir::LLVM::LLVMPointerType::get(&ctx.context);
        auto addrOp = ctx.builder.create<mlir::LLVM::AddressOfOp>(loc, ptrType, globalName);
        auto lenOp  = ctx.builder.create<mlir::arith::ConstantIntOp>(
            loc, static_cast<int64_t>(node->value.size()), 64);

        auto structType = mlir::LLVM::LLVMStructType::getLiteral(
            &ctx.context, {ptrType, ctx.builder.getI64Type()});
        auto undef     = ctx.builder.create<mlir::LLVM::UndefOp>(loc, structType);
        auto insertPtr = ctx.builder.create<mlir::LLVM::InsertValueOp>(
            loc, undef, addrOp, ctx.builder.getDenseI64ArrayAttr({0}));
        lastValue      = ctx.builder.create<mlir::LLVM::InsertValueOp>(
            loc, insertPtr, lenOp, ctx.builder.getDenseI64ArrayAttr({1}));
    }

    void visit(Identifier* node) override {
        auto loc = ctx.getLoc(node->line, node->column);
        mlir::Value alloca = scopeManager.lookupVariable(node->name);
        if (alloca) {
            auto memRefType = mlir::dyn_cast<mlir::MemRefType>(alloca.getType());
            if (memRefType && memRefType.getRank() > 0) {
                // Dizi değişkeni → memref'in kendisi
                lastValue = alloca;
            } else {
                // Skaler → yükle
                lastValue = ctx.builder.create<mlir::memref::LoadOp>(loc, alloca);
            }
        }
    }

    void visit(AssignmentStmt* node) override {
        auto loc = ctx.getLoc(node->line, node->column);
        // RHS'yi hesapla
        node->value->accept(*this);
        mlir::Value val = lastValue;

        if (auto* ident = dynamic_cast<Identifier*>(node->target.get())) {
            // Doğrudan değişkene atama
            mlir::Value alloca = scopeManager.lookupVariable(ident->name);
            if (!alloca) {
                // Hata: tanımsız değişken (ileride emitError eklenebilir)
                return;
            }
            ctx.builder.create<mlir::memref::StoreOp>(loc, val, alloca);
        }
        else if (auto* arrAccess = dynamic_cast<ArrayAccessExpr*>(node->target.get())) {
            // Dizi elemanına atama
            arrAccess->target->accept(*this); // dizinin memref'ini al
            mlir::Value memref = lastValue;
            arrAccess->index->accept(*this); // indeks
            mlir::Value index = lastValue;

            // İndeksi Index tipine çevir (gerekirse)
            auto idxType = mlir::IndexType::get(&ctx.context);
            if (index.getType() != idxType) {
                index = ctx.builder.create<mlir::arith::IndexCastOp>(loc, idxType, index);
            }
            ctx.builder.create<mlir::memref::StoreOp>(loc, val, memref, mlir::ValueRange{index});
        }
    }



    void visit(ListLiteral* node) override {}
    void visit(ArrayAccessExpr* node) override {
        auto loc = ctx.getLoc(node->line, node->column);
        node->target->accept(*this);  // memref değeri
        mlir::Value memref = lastValue;
        node->index->accept(*this);   // indeks
        mlir::Value index = lastValue;

        auto idxType = mlir::IndexType::get(&ctx.context);
        if (index.getType() != idxType) {
            index = ctx.builder.create<mlir::arith::IndexCastOp>(loc, idxType, index);
        }
        lastValue = ctx.builder.create<mlir::memref::LoadOp>(loc, memref, mlir::ValueRange{index});
    }
    void visit(FunctionCallExpr* node) override { /* TODO */ }

    void visit(UnaryExpr* node) override {
        node->operand->accept(*this);
        mlir::Value opVal = lastValue;
        lastValue = arithManager.buildUnary(node->op, opVal, ctx.getLoc(node->line, node->column));
    }

    void visit(BinaryExpr* node) override {
        node->left->accept(*this);
        mlir::Value left = lastValue;
        node->right->accept(*this);
        mlir::Value right = lastValue;
        lastValue = arithManager.buildBinary(node->op, left, right,
                                              ctx.getLoc(node->line, node->column));
    }

    void visit(BlockStmt* node) override {
        scopeManager.pushScope();
        for (auto& stmt : node->statements) {
            stmt->accept(*this);
        }
        scopeManager.popScope();
    }

    void visit(ExprStmt* node) override {
        node->expr->accept(*this);
    }

    void visit(VariableDecl* node) override {
        auto loc = ctx.getLoc(node->line, node->column);

        mlir::Value initVal;
        if (node->initializer) {
            node->initializer->accept(*this);
            initVal = lastValue;
        } else {
            mlir::Type defaultType = typeManager.convertType(node->type.get());
            if (defaultType) {
                if (auto intTy = mlir::dyn_cast<mlir::IntegerType>(defaultType)) {
                    initVal = ctx.builder.create<mlir::arith::ConstantIntOp>(loc, 0, intTy.getWidth());
                } else if (mlir::isa<mlir::FloatType>(defaultType)) {
                    initVal = ctx.builder.create<mlir::arith::ConstantFloatOp>(
                        loc, llvm::APFloat(0.0), llvm::cast<mlir::FloatType>(defaultType));
                } else if (defaultType.isInteger(1)) {
                    initVal = ctx.builder.create<mlir::arith::ConstantIntOp>(loc, 0, 1);
                }
            }
        }

        mlir::Type type = typeManager.convertType(node->type.get());
        if (!type && initVal) type = initVal.getType();

        // Dizi ise boyutlu MemRef, değilse skalar (rank-0)
        mlir::MemRefType memRefType;
        if (node->type && node->type->arraySize > 0) {
            memRefType = mlir::MemRefType::get({node->type->arraySize}, type);
        } else {
            memRefType = mlir::MemRefType::get({}, type);
        }

        mlir::Value alloca = ctx.builder.create<mlir::memref::AllocaOp>(loc, memRefType);
        if (initVal) {
            ctx.builder.create<mlir::memref::StoreOp>(loc, initVal, alloca);
        }
        scopeManager.declareVariable(node->name, alloca);
    }

    void visit(ParamDecl* node) override {}

    void visit(FunctionDecl* node) override {
        auto loc = ctx.getLoc(node->line, node->column);

        llvm::SmallVector<mlir::Type, 4> argTypes;
        for (auto& p : node->params) {
            argTypes.push_back(typeManager.convertType(p->type.get()));
        }

        mlir::Type retType = typeManager.convertType(node->returnType.get());

        // Build result type list (empty for void)
        llvm::SmallVector<mlir::Type, 1> resultTypes;
        bool isVoid = !retType || mlir::isa<mlir::NoneType>(retType);
        if (!isVoid) resultTypes.push_back(retType);

        auto funcType = ctx.builder.getFunctionType(
            mlir::TypeRange(argTypes),
            mlir::TypeRange(resultTypes));

        auto funcOp = ctx.builder.create<mlir::func::FuncOp>(loc, node->name, funcType);

        mlir::Block* entryBlock = funcOp.addEntryBlock();
        mlir::OpBuilder::InsertionGuard guard(ctx.builder);
        ctx.builder.setInsertionPointToStart(entryBlock);

        scopeManager.pushScope();
        for (size_t i = 0; i < node->params.size(); ++i) {
            mlir::Value arg = entryBlock->getArgument(static_cast<unsigned>(i));
            auto memRefType = mlir::MemRefType::get({}, argTypes[i]);
            mlir::Value alloca = ctx.builder.create<mlir::memref::AllocaOp>(loc, memRefType);
            ctx.builder.create<mlir::memref::StoreOp>(loc, arg, alloca);
            scopeManager.declareVariable(node->params[i]->name, alloca);
        }

        node->body->accept(*this);

        // Emit implicit return for void functions
        if (isVoid) {
            ctx.builder.create<mlir::func::ReturnOp>(loc, mlir::ValueRange());
        }

        scopeManager.popScope();
    }

    void visit(LambdaExpr* node) override {}

    void visit(IfStmt* node) override {
        auto loc = ctx.getLoc(node->line, node->column);
        node->condition->accept(*this);
        mlir::Value cond = lastValue;

        cfManager.buildIf(cond, loc,
            [&]() { node->trueBlock->accept(*this); },
            node->elseBlock
                ? std::function<void()>([&]() { node->elseBlock->accept(*this); })
                : nullptr);
    }

    void visit(WhileStmt* node) override {
        auto loc = ctx.getLoc(node->line, node->column);
        cfManager.buildWhile(loc,
            [&]() -> mlir::Value {
                node->condition->accept(*this);
                return lastValue;
            },
            [&]() { node->body->accept(*this); });
    }

    void visit(ForRangeStmt* node) override {}
    void visit(ForCollectionStmt* node) override {}
    void visit(BreakStmt* node) override {}
    void visit(ContinueStmt* node) override {}

    void visit(ReturnStmt* node) override {
        auto loc = ctx.getLoc(node->line, node->column);
        if (node->expr) {
            node->expr->accept(*this);
            llvm::SmallVector<mlir::Value, 1> retVals = {lastValue};
            ctx.builder.create<mlir::func::ReturnOp>(loc, mlir::ValueRange(retVals));
        } else {
            ctx.builder.create<mlir::func::ReturnOp>(loc, mlir::ValueRange());
        }
    }

};



#endif //MLIRGENERATOR_H
