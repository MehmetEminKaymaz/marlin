//
// Created by Eminf on 19.07.2026.
//

#ifndef CODEGENCONTEXT_H
#define CODEGENCONTEXT_H

#include "mlir/IR/Builders.h"
#include "mlir/IR/BuiltinOps.h"
#include "mlir/IR/Location.h"
#include "mlir/Dialect/MemRef/IR/MemRef.h"
#include <unordered_map>
#include <vector>
#include <string>

struct CodegenContext {
    mlir::OpBuilder& builder;
    mlir::MLIRContext& context;
    mlir::ModuleOp theModule;
    std::vector<std::unordered_map<std::string, mlir::Value>> symbolTable;

    // Üretilmekte olan fonksiyonun giriş bloğu. Bütün yığıt tahsisleri buraya
    // yapılır (bkz. createEntryAlloca).
    mlir::Block* allocaBlock = nullptr;

    CodegenContext(mlir::OpBuilder& b, mlir::MLIRContext& c, mlir::ModuleOp m)
        : builder(b), context(c), theModule(m) {
        // Push global scope
        symbolTable.push_back(std::unordered_map<std::string, mlir::Value>());
    }

    mlir::Location getLoc(int line, int col) {
        return mlir::FileLineColLoc::get(&context, "main.src", line, col);
    }

    // Tahsisi, o an neresi üretiliyor olursa olsun fonksiyonun giriş bloğuna
    // koyar. Döngü gövdesinde üretilen bir alloca her yinelemede yığıtı
    // büyütürdü; ayrıca tek bloklu scf bölgelerinin içinden de her zaman
    // erişilebilir (giriş bloğu her şeye hâkimdir).
    mlir::Value createEntryAlloca(mlir::Location loc, mlir::MemRefType type) {
        mlir::OpBuilder::InsertionGuard guard(builder);
        builder.setInsertionPointToStart(allocaBlock);
        return builder.create<mlir::memref::AllocaOp>(loc, type);
    }
};

#endif //CODEGENCONTEXT_H
