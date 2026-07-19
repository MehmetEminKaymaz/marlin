//
// Created by Eminf on 19.07.2026.
//

#ifndef CODEGENCONTEXT_H
#define CODEGENCONTEXT_H

#include "mlir/IR/Builders.h"
#include "mlir/IR/BuiltinOps.h"
#include "mlir/IR/Location.h"
#include <unordered_map>
#include <vector>
#include <string>

struct CodegenContext {
    mlir::OpBuilder& builder;
    mlir::MLIRContext& context;
    mlir::ModuleOp theModule;
    std::vector<std::unordered_map<std::string, mlir::Value>> symbolTable;

    CodegenContext(mlir::OpBuilder& b, mlir::MLIRContext& c, mlir::ModuleOp m)
        : builder(b), context(c), theModule(m) {
        // Push global scope
        symbolTable.push_back(std::unordered_map<std::string, mlir::Value>());
    }

    mlir::Location getLoc(int line, int col) {
        return mlir::FileLineColLoc::get(&context, "main.src", line, col);
    }
};

#endif //CODEGENCONTEXT_H
