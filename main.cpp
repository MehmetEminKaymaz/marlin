#include <iostream>
#include "mlir/IR/MLIRContext.h"
#include "mlir/IR/Builders.h"
#include "mlir/IR/BuiltinOps.h"
#include "llvm/Support/raw_ostream.h"
#include "minikLexer.h"
#include "minikParser.h"
#include "minikBaseVisitor.h"
#include "antlr4-runtime.h"

using namespace antlr4;

int main() {
    // 1. MLIR Context yapısını oluşturuyoruz
    mlir::MLIRContext context;

    mlir::OpBuilder builder(&context);

    mlir::Location location = mlir::FileLineColLoc::get(&context, "main.minik",1,5);

    std::string program = "int x = 10;";

    // ANTLR kurulumu
    ANTLRInputStream input(program);
    minikLexer lexer(&input);
    CommonTokenStream tokens(&lexer);
    minikParser parser(&tokens);

    // 'program' kuralından ağacı oluştur (Gramerindeki ilk kural 'program')
    tree::ParseTree* tree = parser.program();

    std::cout << "Parse sonucu: " << tree->toStringTree(&parser) << std::endl;

    return 0;
}