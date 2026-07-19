//
// Created by Eminf on 19.07.2026.
//

#ifndef MLIRSCOPEMANAGER_H
#define MLIRSCOPEMANAGER_H

#include "CodegenContext.h"
#include <string>
#include <iostream>

class MLIRScopeManager {
    CodegenContext& ctx;
public:
    MLIRScopeManager(CodegenContext& context) : ctx(context) {}

    void pushScope() {
        ctx.symbolTable.push_back(std::unordered_map<std::string, mlir::Value>());
    }

    void popScope() {
        if (ctx.symbolTable.size() > 1) {
            ctx.symbolTable.pop_back();
        }
    }

    void declareVariable(const std::string& name, mlir::Value val) {
        ctx.symbolTable.back()[name] = val;
    }

    mlir::Value lookupVariable(const std::string& name) {
        for (auto it = ctx.symbolTable.rbegin(); it != ctx.symbolTable.rend(); ++it) {
            if (it->count(name)) {
                return it->at(name);
            }
        }
        std::cerr << "Variable not found: " << name << "\n";
        return nullptr;
    }
};



#endif //MLIRSCOPEMANAGER_H
