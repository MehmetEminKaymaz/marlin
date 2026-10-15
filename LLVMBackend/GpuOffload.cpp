#include "GpuOffload.h"

#include "../MLIRBackend/CodegenContext.h" // kGpuFunctionAttr

#include "llvm/ADT/SetVector.h"
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

// Matris çarpımı çekirdeğinin döşemeleri (bkz. GpuOffload.h, 1).
// Bir blok sonucun kTile x kTile'lık bir parçasını hesaplar; bloktaki
// kThreads x kThreads iş parçacığının her biri bunun kPerThread x kPerThread
// elemanını yazmaçlarda biriktirir. Girdiler kTileK genişliğinde şeritler
// halinde paylaşımlı belleğe alınır.
constexpr int64_t kThreads = 16;
constexpr int64_t kPerThread = 4;
constexpr int64_t kTile = kThreads * kPerThread; // 64
constexpr int64_t kTileK = 16;

class GpuOffloadPass : public PassWrapper<GpuOffloadPass, OperationPass<ModuleOp>> {
public:
    MLIR_DEFINE_EXPLICIT_INTERNAL_INLINE_TYPE_ID(GpuOffloadPass)

    StringRef getArgument() const final { return "marlin-gpu-offload"; }
    StringRef getDescription() const final {
        return "marlin.gpu isaretli fonksiyonlardaki paralel isleri GPU cekirdeklerine cevirir";
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
    LogicalResult convertMatmul(linalg::MatmulOp op);
    LogicalResult convertToLaunch(scf::ParallelOp loop);
    LogicalResult prepareKernel(gpu::LaunchOp launch, Operation* hoistBefore);
};

LogicalResult GpuOffloadPass::offload(func::FuncOp function) {
    // 1a. Matris çarpımı → kendi döşemeli çekirdeği.
    llvm::SmallVector<linalg::MatmulOp, 4> products;
    function.walk([&](linalg::MatmulOp op) { products.push_back(op); });
    for (linalg::MatmulOp op : products) {
        if (failed(convertMatmul(op))) return failure();
        op->erase();
    }

    // 1b. Kalan linalg işlemleri → paralel döngüler.
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

// C += A * B (linalg.matmul; C kod üretiminde sıfırlanmıştır).
//
//   gpu.launch blocks(ceil(N / 64), ceil(M / 64)) threads(16, 16)
//              workgroup(As: 64 x 16, Bs: 16 x 64) {
//     acc[4][4] = 0                                    yazmaçlarda
//     for k0 = 0 to K step 16 {
//       As = A[blok satırları, k0 .. k0+16]            her iş parçacığı 4 eleman
//       Bs = B[k0 .. k0+16, blok sütunları]            taşar: 0
//       barrier
//       for kk = 0 to 16 { a[r] = As[ty + 16r, kk]; b[c] = Bs[kk, tx + 16c]
//                          acc[r][c] += a[r] * b[c] }
//       barrier
//     }
//     C[satır, sütun] += acc[r][c]                     matrisin içindekiler
//   }
//
// İş parçacığının satırları ty, ty+16, ... ve sütunları tx, tx+16, ...:
// bir warp'taki komşu iş parçacıkları komşu sütunlara yazar (birleşik bellek
// erişimi) ve paylaşımlı bellekte farklı bankalardan okur.
LogicalResult GpuOffloadPass::convertMatmul(linalg::MatmulOp op) {
    if (!op.hasPureBufferSemantics())
        return op.emitError("GPU matris carpimi bellek uzerinde olmali");
    Value lhs = op.getDpsInputOperand(0)->get();
    Value rhs = op.getDpsInputOperand(1)->get();
    Value out = op.getDpsInitOperand(0)->get();
    Type elementType = cast<MemRefType>(out.getType()).getElementType();
    const bool isFloat = isa<FloatType>(elementType);
    if (!isFloat && !isa<IntegerType>(elementType))
        return op.emitError("GPU matris carpimi bu eleman tipini desteklemiyor");

    MLIRContext* context = op.getContext();
    Location loc = op.getLoc();
    OpBuilder b(op);
    auto constant = [&](int64_t value) -> Value {
        return b.create<arith::ConstantIndexOp>(loc, value);
    };
    // Çarpma ile toplama tek işlem (FMA) olabilir; CUDA'nın varsayılanı da budur.
    auto contract = arith::FastMathFlagsAttr::get(context, arith::FastMathFlags::contract);
    auto multiplyAdd = [&](Value acc, Value x, Value y) -> Value {
        if (isFloat)
            return b.create<arith::AddFOp>(loc, acc, b.create<arith::MulFOp>(loc, x, y, contract),
                                           contract);
        return b.create<arith::AddIOp>(loc, acc, b.create<arith::MulIOp>(loc, x, y));
    };
    auto lessThan = [&](Value x, Value y) {
        return b.create<arith::CmpIOp>(loc, arith::CmpIPredicate::ult, x, y);
    };
    auto both = [&](Value x, Value y) { return b.create<arith::AndIOp>(loc, x, y); };

    Value zero = constant(0), one = constant(1);
    Value rows = b.create<memref::DimOp>(loc, lhs, 0);
    Value depth = b.create<memref::DimOp>(loc, lhs, 1);
    Value cols = b.create<memref::DimOp>(loc, rhs, 1);

    // Sonuç boşsa çekirdek başlatılmaz (GPU sıfır blokla başlatılamaz).
    Value nonEmpty = both(b.create<arith::CmpIOp>(loc, arith::CmpIPredicate::sgt, rows, zero),
                          b.create<arith::CmpIOp>(loc, arith::CmpIPredicate::sgt, cols, zero));
    auto ifNonEmpty = b.create<scf::IfOp>(loc, nonEmpty, /*withElseRegion=*/false);
    b.setInsertionPointToStart(ifNonEmpty.thenBlock());

    Value tile = constant(kTile);
    auto workgroup = gpu::AddressSpaceAttr::get(context, gpu::AddressSpace::Workgroup);
    auto lhsTileType = MemRefType::get({kTile, kTileK}, elementType, MemRefLayoutAttrInterface(),
                                       workgroup);
    auto rhsTileType = MemRefType::get({kTileK, kTile}, elementType, MemRefLayoutAttrInterface(),
                                       workgroup);
    Type tileTypes[] = {lhsTileType, rhsTileType};
    auto launch = b.create<gpu::LaunchOp>(
        loc, b.create<arith::CeilDivUIOp>(loc, cols, tile),
        b.create<arith::CeilDivUIOp>(loc, rows, tile), one, constant(kThreads),
        constant(kThreads), one, /*dynamicSharedMemorySize=*/Value(), /*asyncTokenType=*/Type(),
        /*asyncDependencies=*/ValueRange(), /*workgroupAttributions=*/TypeRange(tileTypes));
    Block* kernel = &launch.getBody().front();
    b.setInsertionPointToEnd(kernel);
    Value lhsTile = launch.getWorkgroupAttributions()[0];
    Value rhsTile = launch.getWorkgroupAttributions()[1];

    // Çekirdeğin içinde kullanılan sabitler çekirdekte üretilir.
    zero = constant(0);
    one = constant(1);
    tile = constant(kTile);
    Value threads = constant(kThreads);
    Value tx = launch.getThreadIds().x, ty = launch.getThreadIds().y;
    Value rowBase = b.create<arith::MulIOp>(loc, launch.getBlockIds().y, constant(kTile));
    Value colBase = b.create<arith::MulIOp>(loc, launch.getBlockIds().x, constant(kTile));
    Value thread = b.create<arith::AddIOp>(loc, b.create<arith::MulIOp>(loc, ty, threads), tx);
    Value zeroElement = b.create<arith::ConstantOp>(loc, b.getZeroAttr(elementType));

    // matrix[row, col], sınır dışındaysa 0.
    auto loadOrZero = [&](Value matrix, Value row, Value col, Value rowLimit, Value colLimit) {
        auto inside = b.create<scf::IfOp>(
            loc, both(lessThan(row, rowLimit), lessThan(col, colLimit)),
            [&](OpBuilder& nb, Location l) {
                nb.create<scf::YieldOp>(
                    l, nb.create<memref::LoadOp>(l, matrix, ValueRange{row, col}).getResult());
            },
            [&](OpBuilder& nb, Location l) { nb.create<scf::YieldOp>(l, zeroElement); });
        return inside.getResult(0);
    };

    // Her iş parçacığı bir şeridin (kTile * kTileK eleman) bu kadarını taşır.
    constexpr int64_t kLoadsPerThread = kTile * kTileK / (kThreads * kThreads);
    llvm::SmallVector<Value, kPerThread * kPerThread> initial(kPerThread * kPerThread, zeroElement);
    auto stripLoop = b.create<scf::ForOp>(loc, zero, depth, constant(kTileK), initial);
    {
        OpBuilder::InsertionGuard guard(b);
        b.setInsertionPointToStart(stripLoop.getBody());
        Value k0 = stripLoop.getInductionVar();
        for (int64_t q = 0; q < kLoadsPerThread; ++q) {
            Value element = b.create<arith::AddIOp>(loc, thread, constant(q * kThreads * kThreads));
            // A şeridi kTile x kTileK, B şeridi kTileK x kTile; ikisi de satır satır.
            Value lhsRow = b.create<arith::DivUIOp>(loc, element, constant(kTileK));
            Value lhsCol = b.create<arith::RemUIOp>(loc, element, constant(kTileK));
            Value value = loadOrZero(lhs, b.create<arith::AddIOp>(loc, rowBase, lhsRow),
                                     b.create<arith::AddIOp>(loc, k0, lhsCol), rows, depth);
            b.create<memref::StoreOp>(loc, value, lhsTile, ValueRange{lhsRow, lhsCol});

            Value rhsRow = b.create<arith::DivUIOp>(loc, element, tile);
            Value rhsCol = b.create<arith::RemUIOp>(loc, element, tile);
            value = loadOrZero(rhs, b.create<arith::AddIOp>(loc, k0, rhsRow),
                               b.create<arith::AddIOp>(loc, colBase, rhsCol), depth, cols);
            b.create<memref::StoreOp>(loc, value, rhsTile, ValueRange{rhsRow, rhsCol});
        }
        b.create<gpu::BarrierOp>(loc);

        auto innerLoop = b.create<scf::ForOp>(loc, zero, constant(kTileK), one,
                                              stripLoop.getRegionIterArgs());
        {
            OpBuilder::InsertionGuard innerGuard(b);
            b.setInsertionPointToStart(innerLoop.getBody());
            Value kk = innerLoop.getInductionVar();
            llvm::SmallVector<Value, kPerThread> lhsValues, rhsValues;
            for (int64_t r = 0; r < kPerThread; ++r) {
                Value row = b.create<arith::AddIOp>(loc, ty, constant(r * kThreads));
                lhsValues.push_back(b.create<memref::LoadOp>(loc, lhsTile, ValueRange{row, kk}));
                Value col = b.create<arith::AddIOp>(loc, tx, constant(r * kThreads));
                rhsValues.push_back(b.create<memref::LoadOp>(loc, rhsTile, ValueRange{kk, col}));
            }
            llvm::SmallVector<Value, kPerThread * kPerThread> sums;
            for (int64_t r = 0; r < kPerThread; ++r)
                for (int64_t c = 0; c < kPerThread; ++c)
                    sums.push_back(multiplyAdd(innerLoop.getRegionIterArgs()[r * kPerThread + c],
                                               lhsValues[r], rhsValues[c]));
            b.create<scf::YieldOp>(loc, sums);
        }
        // Şerit bir sonraki turda üzerine yazılmadan önce herkes işini bitirmeli.
        b.create<gpu::BarrierOp>(loc);
        b.create<scf::YieldOp>(loc, innerLoop.getResults());
    }

    for (int64_t r = 0; r < kPerThread; ++r) {
        Value row = b.create<arith::AddIOp>(loc, rowBase,
                                            b.create<arith::AddIOp>(loc, ty, constant(r * kThreads)));
        for (int64_t c = 0; c < kPerThread; ++c) {
            Value col = b.create<arith::AddIOp>(
                loc, colBase, b.create<arith::AddIOp>(loc, tx, constant(c * kThreads)));
            auto inside = b.create<scf::IfOp>(loc, both(lessThan(row, rows), lessThan(col, cols)),
                                              /*withElseRegion=*/false);
            OpBuilder::InsertionGuard guard(b);
            b.setInsertionPointToStart(inside.thenBlock());
            Value old = b.create<memref::LoadOp>(loc, out, ValueRange{row, col});
            Value sum = stripLoop.getResult(r * kPerThread + c);
            Value updated = isFloat ? b.create<arith::AddFOp>(loc, old, sum).getResult()
                                    : b.create<arith::AddIOp>(loc, old, sum).getResult();
            b.create<memref::StoreOp>(loc, updated, out, ValueRange{row, col});
        }
    }
    b.create<gpu::TerminatorOp>(loc);
    return prepareKernel(launch, ifNonEmpty);
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

// Çekirdek gövdelerini GPU derleyicisi için son haline getirir
// (bkz. GpuOffload.h, createGpuKernelPreparePass).
class GpuKernelPreparePass
    : public PassWrapper<GpuKernelPreparePass, OperationPass<ModuleOp>> {
public:
    MLIR_DEFINE_EXPLICIT_INTERNAL_INLINE_TYPE_ID(GpuKernelPreparePass)

    StringRef getArgument() const final { return "marlin-gpu-kernel-prepare"; }
    StringRef getDescription() const final {
        return "sabitleri GPU cekirdeklerine tasir, carpma-toplamayi birlestirmeye izin verir";
    }

    void runOnOperation() override {
        getOperation().walk([&](gpu::LaunchOp launch) {
            sinkConstants(launch);
            allowContraction(launch);
        });
    }

private:
    // Çekirdeğin dışında üretilip içinde kullanılan sabitler çekirdeğin
    // başına kopyalanır. Dışarıda kalsalar çekirdeğe parametre olarak
    // geçerlerdi ve GPU derleyicisi değerlerini bilemezdi (ör. döngü adımının
    // 1 olduğunu bilmeyen derleyici döngüyü açamaz, adres hesabını sadeleştiremez).
    static void sinkConstants(gpu::LaunchOp launch) {
        Region& body = launch.getBody();
        llvm::SetVector<Operation*> constants;
        body.walk([&](Operation* op) {
            for (Value operand : op->getOperands()) {
                auto constant = operand.getDefiningOp<arith::ConstantOp>();
                if (constant && !launch->isAncestor(constant)) constants.insert(constant);
            }
        });
        OpBuilder b(&body.front(), body.front().begin());
        for (Operation* constant : constants) {
            Operation* copy = b.clone(*constant);
            constant->getResult(0).replaceUsesWithIf(copy->getResult(0), [&](OpOperand& use) {
                return launch->isProperAncestor(use.getOwner());
            });
        }
    }

    // Kayan noktalı çarpma ve toplamaya `contract` izni: GPU derleyicisi
    // a * b + c'yi tek bir FMA komutuyla hesaplayabilir (CUDA'nın varsayılanı).
    // Sonuç ara yuvarlama yapılmadığı için son basamakta farklı olabilir.
    static void allowContraction(gpu::LaunchOp launch) {
        launch.getBody().walk([](Operation* op) {
            auto fastMath = dyn_cast<arith::ArithFastMathInterface>(op);
            if (!fastMath || !isa<arith::MulFOp, arith::AddFOp, arith::SubFOp>(op)) return;
            arith::FastMathFlags flags =
                fastMath.getFastMathFlagsAttr().getValue() | arith::FastMathFlags::contract;
            op->setAttr(fastMath.getFastMathAttrName(),
                        arith::FastMathFlagsAttr::get(op->getContext(), flags));
        });
    }
};

} // namespace

std::unique_ptr<Pass> marlin::createGpuOffloadPass() {
    return std::make_unique<GpuOffloadPass>();
}

std::unique_ptr<Pass> marlin::createGpuKernelPreparePass() {
    return std::make_unique<GpuKernelPreparePass>();
}
