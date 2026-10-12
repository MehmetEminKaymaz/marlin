#include "ParallelMatmul.h"

#include "../MLIRBackend/CodegenContext.h" // kGpuFunctionAttr

#include "llvm/ADT/SmallVector.h"
#include "mlir/Dialect/Affine/IR/AffineOps.h"
#include "mlir/Dialect/Func/IR/FuncOps.h"
#include "mlir/Dialect/Linalg/IR/Linalg.h"
#include "mlir/Dialect/Linalg/Transforms/Transforms.h"
#include "mlir/Dialect/MemRef/IR/MemRef.h"
#include "mlir/Dialect/SCF/IR/SCF.h"
#include "mlir/IR/PatternMatch.h"
#include "mlir/Pass/Pass.h"

using namespace mlir;

namespace {

class ParallelMatmulPass : public PassWrapper<ParallelMatmulPass, OperationPass<ModuleOp>> {
public:
    MLIR_DEFINE_EXPLICIT_INTERNAL_INLINE_TYPE_ID(ParallelMatmulPass)

    StringRef getArgument() const final { return "minik-parallel-matmul"; }
    StringRef getDescription() const final {
        return "CPU fonksiyonlarindaki linalg.matmul islemlerini paralel donguye acar";
    }
    void getDependentDialects(DialectRegistry& registry) const override {
        registry.insert<scf::SCFDialect, memref::MemRefDialect, affine::AffineDialect>();
    }

    void runOnOperation() override {
        for (func::FuncOp function : getOperation().getOps<func::FuncOp>()) {
            if (function.isExternal() || function->hasAttr(kGpuFunctionAttr)) continue;

            llvm::SmallVector<linalg::MatmulOp, 4> products;
            function.walk([&](linalg::MatmulOp op) { products.push_back(op); });

            IRRewriter rewriter(function.getContext());
            for (linalg::MatmulOp op : products) {
                rewriter.setInsertionPoint(op);
                if (failed(linalg::linalgOpToParallelLoops(rewriter, op))) {
                    op.emitError("matris carpimi paralel donguye acilamadi");
                    return signalPassFailure();
                }
                rewriter.eraseOp(op);
            }
        }
    }
};

} // namespace

std::unique_ptr<Pass> minik::createParallelMatmulPass() {
    return std::make_unique<ParallelMatmulPass>();
}
