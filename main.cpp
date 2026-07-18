#include <iostream>
#include "mlir/IR/MLIRContext.h"
#include "mlir/IR/Builders.h"
#include "mlir/IR/BuiltinOps.h"
#include "llvm/Support/raw_ostream.h"

int main() {
    // 1. MLIR Context yapısını oluşturuyoruz
    mlir::MLIRContext context;

    mlir::OpBuilder builder(&context);

    mlir::Location location = mlir::FileLineColLoc::get(&context, "main.minik",1,5);

    return 0;
}