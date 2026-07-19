#include <iostream>
#include "mlir/IR/MLIRContext.h"
#include "mlir/IR/Builders.h"
#include "mlir/IR/BuiltinOps.h"
#include "llvm/Support/raw_ostream.h"
#include "minikLexer.h"
#include "minikParser.h"
#include "minikBaseVisitor.h"
#include "antlr4-runtime.h"
#include "AST/ASTBuilder.h"
#include "AST/ASTPrinter.h"

#include "MLIRBackend/MLIRGenerator.h"
#include <mlir/IR/MLIRContext.h>
#include <mlir/IR/BuiltinOps.h>
#include <mlir/IR/Builders.h>

#include "AST/ASTValidator.h"
#include "mlir/IR/BuiltinTypes.h"
#include "mlir/Dialect/Func/IR/FuncOps.h"
#include "mlir/Dialect/Arith/IR/Arith.h"
#include "mlir/IR/Verifier.h"
#include "mlir/Support/LogicalResult.h"
#include "llvm/IR/LLVMContext.h"
#include "mlir/Target/LLVMIR/ModuleTranslation.h"
#include "mlir/Target/LLVMIR/Dialect/All.h"
#include "mlir/Target/LLVMIR/Export.h"
#include "mlir/Dialect/LLVMIR/LLVMDialect.h"
#include "mlir/Pass/Pass.h"
#include "mlir/Pass/PassManager.h"
#include "mlir/Target/LLVMIR/Export.h"
#include "mlir/Conversion/FuncToLLVM/ConvertFuncToLLVM.h"
#include "mlir/Conversion/FuncToLLVM/ConvertFuncToLLVMPass.h"
#include "mlir/Conversion/ArithToLLVM/ArithToLLVM.h"
#include "mlir/Dialect/MemRef/IR/MemRef.h"
#include "mlir/ExecutionEngine/ExecutionEngine.h"
#include "mlir/ExecutionEngine/OptUtils.h"
#include "mlir/Target/LLVMIR/Dialect/LLVMIR/LLVMToLLVMIRTranslation.h"
#include "mlir/Target/LLVMIR/Export.h"
#include "llvm/Support/TargetSelect.h"
#include "llvm/Support/FileSystem.h"
#include "mlir/Conversion/ReconcileUnrealizedCasts/ReconcileUnrealizedCasts.h"
#include "mlir/Conversion/MemRefToLLVM/MemRefToLLVM.h"

using namespace antlr4;

int main() {
    std::string sourceCode =
        "func main() -> void {"
    "let x = 10 + 20 * 5;"
    "int y = x - 5;"
    "int[5] dizi = [1,2,3,4];"
    "list<int> dizi2 = [1,2,3,4];"
    "int result;"
    "if y < 100 {"
    "result = x + 3;"
    "y = y + 1;"
    "dizi[2] = 4;"
    "}"
    "int total;"
    "for i in 0 .. 100 step 2 { total = total + i; }"
    "for sayi in dizi { total = total + sayi;}"
    "while total < 1000 { total = total + 1; }"
    "}"
    "func myfunc() -> void { }"
    ;

    std::cout << "--- Kaynak Kod ---\n" << sourceCode << "\n";

    ANTLRInputStream input(sourceCode);
    minikLexer lexer(&input);
    CommonTokenStream tokens(&lexer);
    minikParser parser(&tokens);

    minikParser::ProgramContext* tree = parser.program();

    if (parser.getNumberOfSyntaxErrors() > 0) {
        std::cerr << "Syntax hatasi bulundu! AST olusturulmayacak.\n";
        return 1;
    }

    ASTBuilder builder;
    std::any astResult = builder.visit(tree);

    if (astResult.has_value()) {
        std::unique_ptr<Program> astRoot(std::any_cast<Program*>(astResult));

        ASTPrinter printer;
        astRoot->accept(printer);

        ASTValidator validator;
        astRoot->accept(validator);
        if (validator.hasErrors()) {
            for (const auto& err : validator.getErrors())
                llvm::errs() << err << "\n";
            return 1;
        }

        mlir::DialectRegistry registry;

        registry.insert<mlir::func::FuncDialect>();
        registry.insert<mlir::arith::ArithDialect>();
        registry.insert<mlir::LLVM::LLVMDialect>();
        registry.insert<mlir::memref::MemRefDialect>();

        mlir::registerAllToLLVMIRTranslations(registry);

        mlir::MLIRContext mlirCtx;
        mlirCtx.getOrLoadDialect<mlir::func::FuncDialect>();
        mlirCtx.getOrLoadDialect<mlir::arith::ArithDialect>();
        mlirCtx.getOrLoadDialect<mlir::scf::SCFDialect>();
        mlirCtx.getOrLoadDialect<mlir::memref::MemRefDialect>();
        mlirCtx.getOrLoadDialect<mlir::LLVM::LLVMDialect>();

        mlir::OpBuilder mlirBuilder(&mlirCtx);
        mlir::ModuleOp moduleOp = mlir::ModuleOp::create(mlirBuilder.getUnknownLoc());

        MLIRGenerator generator(mlirCtx, mlirBuilder, moduleOp);
        astRoot->accept(generator);

        // Sonucu yazdır
        std::cout << "\n=== Generated MLIR ===" << std::endl;
        moduleOp->dump();

        /*mlir::PassManager pm(&mlirCtx);

        pm.addPass(mlir::createArithToLLVMConversionPass());
        pm.addPass(mlir::createConvertFuncToLLVMPass());
        pm.addPass(mlir::createReconcileUnrealizedCastsPass());
        pm.addPass(mlir::createFinalizeMemRefToLLVMConversionPass());

        if (mlir::failed(pm.run(moduleOp))) {
            std::cerr << "Pass manager failed!" << std::endl;
            return -1;
        }*/
    }

    return 0;
}