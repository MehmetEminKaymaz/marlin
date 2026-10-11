#include "GpuOffload.h"

#include "../MLIRBackend/CodegenContext.h" // kGpuFunctionAttr

#include "llvm/ADT/SmallVector.h"
#include "mlir/Dialect/Affine/IR/AffineOps.h"
#include "mlir/Dialect/Arith/IR/Arith.h"
#include "mlir/Dialect/Func/IR/FuncOps.h"
#include "mlir/Dialect/GPU/IR/GPUDialect.h"
#include "mlir/Dialect/Linalg/IR/Linalg.h"
#include "mlir/Dialect/Linalg/Transforms/Transforms.h"
#include "mlir/Dialect/MemRef/IR/MemRef.h"
#include "mlir/Dialect/SCF/IR/SCF.h"
#include "mlir/IR/PatternMatch.h"
#include "mlir/Pass/Pass.h"

using namespace mlir;

namespace {

// Bir GPU bloğundaki iş parçacığı sayısı. GPU'lar iş parçacıklarını 32'lik
// gruplar halinde çalıştırır; 256 yaygın ve güvenli bir seçimdir.
constexpr int64_t kThreadsPerBlock = 256;

class GpuOffloadPass : public PassWrapper<GpuOffloadPass, OperationPass<ModuleOp>> {
public:
    MLIR_DEFINE_EXPLICIT_INTERNAL_INLINE_TYPE_ID(GpuOffloadPass)

    StringRef getArgument() const final { return "minik-gpu-offload"; }
    StringRef getDescription() const final {
        return "minik.gpu isaretli fonksiyonlardaki paralel isleri GPU cekirdeklerine cevirir";
    }
    void getDependentDialects(DialectRegistry& registry) const override {
        registry.insert<gpu::GPUDialect, scf::SCFDialect, arith::ArithDialect,
                        memref::MemRefDialect, affine::AffineDialect>();
    }

    void runOnOperation() override {
        for (func::FuncOp function : getOperation().getOps<func::FuncOp>()) {
            if (function.isExternal() || !function->hasAttr(kGpuFunctionAttr)) continue;
            if (failed(offload(function))) return signalPassFailure();
        }
    }

private:
    LogicalResult offload(func::FuncOp function);
    LogicalResult convertToLaunch(scf::ParallelOp loop);
    LogicalResult prepareKernel(gpu::LaunchOp launch, Operation* hoistBefore);
};

LogicalResult GpuOffloadPass::offload(func::FuncOp function) {
    // 1. linalg → paralel döngüler.
    llvm::SmallVector<linalg::LinalgOp, 8> linalgOps;
    function.walk([&](linalg::LinalgOp op) { linalgOps.push_back(op); });
    IRRewriter rewriter(function.getContext());
    for (linalg::LinalgOp op : linalgOps) {
        rewriter.setInsertionPoint(op);
        if (failed(linalg::linalgOpToParallelLoops(rewriter, op)))
            return op->emitError("matris islemi paralel donguye acilamadi");
        rewriter.eraseOp(op);
    }

    // 2. Paralel döngüler → gpu.launch. (İç içe paralel döngü olmaz: Sema
    //    reddeder, linalg ise iç boyutları sıralı döngü olarak üretir.)
    llvm::SmallVector<scf::ParallelOp, 8> loops;
    function.walk([&](scf::ParallelOp loop) { loops.push_back(loop); });
    for (scf::ParallelOp loop : loops) {
        if (loop->getParentOfType<scf::ParallelOp>())
            return loop.emitError("GPU'da ic ice paralel dongu desteklenmiyor");
        if (failed(convertToLaunch(loop))) return failure();
    }
    return success();
}

LogicalResult GpuOffloadPass::convertToLaunch(scf::ParallelOp loop) {
    if (loop.getNumReductions() != 0)
        return loop.emitError("GPU'da indirgemeli paralel dongu desteklenmiyor");

    Location loc = loop.getLoc();
    OpBuilder b(loop);
    auto lowerBounds = loop.getLowerBound();
    auto upperBounds = loop.getUpperBound();
    auto steps = loop.getStep();
    const unsigned dimensions = static_cast<unsigned>(lowerBounds.size());

    // Her boyutun yineleme sayısı ve hepsinin çarpımı.
    Value zero = b.create<arith::ConstantIndexOp>(loc, 0);
    Value one = b.create<arith::ConstantIndexOp>(loc, 1);
    llvm::SmallVector<Value, 4> counts;
    Value total = one;
    for (unsigned d = 0; d < dimensions; ++d) {
        Value range = b.create<arith::SubIOp>(loc, upperBounds[d], lowerBounds[d]);
        Value count = b.create<arith::CeilDivSIOp>(loc, range, steps[d]);
        count = b.create<arith::MaxSIOp>(loc, count, zero); // boş aralık: 0 yineleme
        counts.push_back(count);
        total = b.create<arith::MulIOp>(loc, total, count);
    }

    // Yapılacak iş yoksa çekirdek hiç başlatılmaz (GPU sıfır blokla başlatılamaz).
    Value threads = b.create<arith::ConstantIndexOp>(loc, kThreadsPerBlock);
    Value blocks = b.create<arith::CeilDivUIOp>(loc, total, threads);
    Value nonEmpty = b.create<arith::CmpIOp>(loc, arith::CmpIPredicate::sgt, total, zero);
    auto ifNonEmpty = b.create<scf::IfOp>(loc, nonEmpty, /*withElseRegion=*/false);

    b.setInsertionPointToStart(ifNonEmpty.thenBlock());
    auto launch = b.create<gpu::LaunchOp>(loc, blocks, one, one, threads, one, one);
    Block* kernel = &launch.getBody().front();
    b.setInsertionPointToEnd(kernel);

    // Bu iş parçacığının sırası; son bloktaki fazlalıklar bir şey yapmaz.
    Value position = b.create<arith::AddIOp>(
        loc, b.create<arith::MulIOp>(loc, launch.getBlockIds().x, launch.getBlockSize().x),
        launch.getThreadIds().x);
    Value inRange = b.create<arith::CmpIOp>(loc, arith::CmpIPredicate::ult, position, total);
    auto ifInRange = b.create<scf::IfOp>(loc, inRange, /*withElseRegion=*/false);
    b.create<gpu::TerminatorOp>(loc);

    // Sıradan döngü değişkenlerine: son boyut en hızlı değişir.
    b.setInsertionPointToStart(ifInRange.thenBlock());
    llvm::SmallVector<Value, 4> inductionVars(dimensions);
    Value rest = position;
    for (unsigned d = dimensions; d-- > 0;) {
        Value index = rest;
        if (d > 0) {
            index = b.create<arith::RemUIOp>(loc, rest, counts[d]);
            rest = b.create<arith::DivUIOp>(loc, rest, counts[d]);
        }
        inductionVars[d] = b.create<arith::AddIOp>(
            loc, lowerBounds[d], b.create<arith::MulIOp>(loc, index, steps[d]));
    }

    // Döngü gövdesini (sonlandırıcısı hariç) çekirdeğe taşı.
    Block* body = loop.getBody();
    for (unsigned d = 0; d < dimensions; ++d)
        body->getArgument(d).replaceAllUsesWith(inductionVars[d]);
    Block* destination = ifInRange.thenBlock();
    destination->getOperations().splice(destination->getTerminator()->getIterator(),
                                        body->getOperations(), body->begin(),
                                        std::prev(body->end()));
    loop.erase();

    return prepareKernel(launch, ifNonEmpty);
}

// Çekirdek gövdesini GPU'da çalışabilir hale getirir (bkz. GpuOffload.h, 3).
// `hoistBefore`: CPU'da kalması gereken işlemlerin taşınacağı yer.
LogicalResult GpuOffloadPass::prepareKernel(gpu::LaunchOp launch, Operation* hoistBefore) {
    Block* kernel = &launch.getBody().front();
    auto definedOutside = [&](Value value) {
        return !launch->isAncestor(value.getParentRegion()->getParentOp());
    };

    llvm::SmallVector<memref::AllocaOp, 8> allocas;
    llvm::SmallVector<memref::LoadOp, 8> hostLoads;
    Operation* hostCall = nullptr;
    launch.getBody().walk([&](Operation* op) {
        if (auto alloca = dyn_cast<memref::AllocaOp>(op)) {
            allocas.push_back(alloca);
        } else if (auto load = dyn_cast<memref::LoadOp>(op)) {
            // Boyutsuz (tek değerlik) bir yuva ve döngünün dışında: CPU'daki
            // bir değişken. Matrisler (boyutlu, GPU belleğinde) burada kalır.
            if (load.getMemRefType().getRank() == 0 && definedOutside(load.getMemRef()))
                hostLoads.push_back(load);
        } else if (auto call = dyn_cast<func::CallOp>(op)) {
            // Gövdesi olmayan fonksiyon: çalışma zamanı kütüphanesi (CPU kodu).
            auto callee = SymbolTable::lookupNearestSymbolFrom<func::FuncOp>(
                op, call.getCalleeAttr());
            if (callee && callee.isExternal()) hostCall = op;
        }
    });

    if (hostCall)
        return hostCall->emitError("GPU'da calisan kod calisma zamani kutuphanesini cagiramaz");

    for (memref::AllocaOp alloca : allocas) {
        if (alloca->getNumOperands() != 0)
            return alloca.emitError("GPU'da degisken boyutlu yigit tahsisi desteklenmiyor");
        alloca->moveBefore(&kernel->front());
    }
    for (memref::LoadOp load : hostLoads)
        load->moveBefore(hoistBefore);
    return success();
}

} // namespace

std::unique_ptr<Pass> minik::createGpuOffloadPass() {
    return std::make_unique<GpuOffloadPass>();
}
