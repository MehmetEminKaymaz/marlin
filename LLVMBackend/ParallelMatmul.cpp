#include "ParallelMatmul.h"

#include "../MLIRBackend/CodegenContext.h" // kGpuFunctionAttr

#include <algorithm>

#include "llvm/ADT/SmallVector.h"
#include "mlir/Dialect/Affine/IR/AffineOps.h"
#include "mlir/Dialect/Arith/IR/Arith.h"
#include "mlir/Dialect/Func/IR/FuncOps.h"
#include "mlir/Dialect/Linalg/IR/Linalg.h"
#include "mlir/Dialect/Linalg/Transforms/Transforms.h"
#include "mlir/Dialect/MemRef/IR/MemRef.h"
#include "mlir/Dialect/SCF/IR/SCF.h"
#include "mlir/Dialect/Vector/IR/VectorOps.h"
#include "mlir/IR/PatternMatch.h"
#include "mlir/Interfaces/FunctionInterfaces.h"
#include "mlir/Pass/Pass.h"

using namespace mlir;

namespace {

// Bir vektör yazmacının bayt genişliği. Hedef makine genel x86-64'tür
// (bkz. NativeCodegen.cpp): SSE2, 16 baytlık yazmaçlar.
constexpr int64_t kVectorBytes = 16;

// Yazmaçlarda biriken sonuç parçası: kTileRows satır x kTileVectors vektör
// (float64 için 6 x 4, float için 6 x 8). 12 yazmaç biriktirir; B'nin iki
// vektörü ve A'nın elemanı için yer kalır (SSE2'de 16 yazmaç vardır).
constexpr int64_t kTileRows = 6;
constexpr int64_t kTileVectors = 2;

// Bir görevin hesapladığı sonuç bloğu ve derinlik şeridi. float64'te A'nın
// şeridi 128 KB, B'nin şeridi 512 KB: ikisi birlikte bir çekirdeğin L2
// önbelleğine sığar ve şerit boyunca oradan okunur.
constexpr int64_t kBlockRows = 64;
constexpr int64_t kBlockCols = 256;
constexpr int64_t kBlockDepth = 256;

bool isPlainMatrix(Value value) {
    auto type = dyn_cast<MemRefType>(value.getType());
    return type && type.getRank() == 2 && type.getLayout().isIdentity() &&
           type.getElementType().isIntOrFloat();
}

// C += A * B (C kod üretiminde sıfırlanmıştır).
//
//   scf.parallel (i0, j0) = (0, 0) to (M, N) step (64, 256) {   görev: bir blok
//     for k0 = 0 to K step 256 {                                 derinlik şeridi
//       for i = i0 to (tam parçalar) step 6 {
//         for j = j0 to (tam parçalar) step 2 * genişlik {
//           acc[6][2] = C[i .. i+6, j .. j+2g]                   vektör yazmaçları
//           for k = k0 to k0 + 256 {
//             b[v] = B[k, j + v*g .. ]                           vektör
//             acc[r][v] += A[i + r, k] * b[v]                    A'nın elemanı
//           }                                                    her şeride yayılır
//           C[i .. i+4, j .. j+2g] = acc
//         }
//         kalan sütunlar: sıradan döngü
//       }
//       kalan satırlar: sıradan döngü
//     }
//   }
//
// Her sonuç elemanına çarpımlar k sırasıyla eklenir; sonuç, döngülerin
// sırası değiştirilmeden yapılan hesapla birebir aynıdır (FMA kullanılmaz).
void buildTiledMatmul(linalg::MatmulOp op) {
    Location loc = op.getLoc();
    OpBuilder b(op);
    Value lhs = op.getDpsInputOperand(0)->get();
    Value rhs = op.getDpsInputOperand(1)->get();
    Value out = op.getDpsInitOperand(0)->get();
    Type elementType = cast<MemRefType>(out.getType()).getElementType();
    const bool isFloat = isa<FloatType>(elementType);
    const int64_t width =
        std::max<int64_t>(1, kVectorBytes * 8 / elementType.getIntOrFloatBitWidth());
    const int64_t tileCols = kTileVectors * width;
    auto vectorType = VectorType::get({width}, elementType);

    auto constant = [&](int64_t value) -> Value {
        return b.create<arith::ConstantIndexOp>(loc, value);
    };
    auto add = [&](Value x, Value y) -> Value { return b.create<arith::AddIOp>(loc, x, y); };
    auto addConstant = [&](Value x, int64_t y) { return y == 0 ? x : add(x, constant(y)); };
    auto minimum = [&](Value x, Value y) -> Value { return b.create<arith::MinUIOp>(loc, x, y); };
    // from'dan başlayıp to'yu aşmayan tam adımların sonu.
    auto fullStepsEnd = [&](Value from, Value to, int64_t step) -> Value {
        Value count = b.create<arith::DivUIOp>(loc, b.create<arith::SubIOp>(loc, to, from),
                                               constant(step));
        return add(from, b.create<arith::MulIOp>(loc, count, constant(step)));
    };
    // Skalerler için de vektörler için de aynı.
    auto multiplyAdd = [&](Value acc, Value x, Value y) -> Value {
        if (isFloat) return b.create<arith::AddFOp>(loc, acc, b.create<arith::MulFOp>(loc, x, y));
        return b.create<arith::AddIOp>(loc, acc, b.create<arith::MulIOp>(loc, x, y));
    };
    // Döngü oluşturur ve gövdesine geçer (gövdeyi çağıran kapatır).
    auto loop = [&](Value from, Value to, int64_t step, ValueRange initial = {}) {
        auto forOp = b.create<scf::ForOp>(loc, from, to, constant(step), initial);
        b.setInsertionPointToStart(forOp.getBody());
        return forOp;
    };

    // Kenarlar: C[satırlar, sütunlar] += A[satırlar, ks] * B[ks, sütunlar]
    // sıradan döngülerle (en içte sütunlar: B ve C satır boyunca okunur).
    auto plainPart = [&](Value rowFrom, Value rowTo, Value colFrom, Value colTo, Value kFrom,
                         Value kTo) {
        OpBuilder::InsertionGuard guard(b);
        Value i = loop(rowFrom, rowTo, 1).getInductionVar();
        Value k = loop(kFrom, kTo, 1).getInductionVar();
        Value a = b.create<memref::LoadOp>(loc, lhs, ValueRange{i, k});
        Value j = loop(colFrom, colTo, 1).getInductionVar();
        Value old = b.create<memref::LoadOp>(loc, out, ValueRange{i, j});
        Value bValue = b.create<memref::LoadOp>(loc, rhs, ValueRange{k, j});
        b.create<memref::StoreOp>(loc, multiplyAdd(old, a, bValue), out, ValueRange{i, j});
    };

    Value zero = constant(0);
    Value rows = b.create<memref::DimOp>(loc, lhs, 0);
    Value depth = b.create<memref::DimOp>(loc, lhs, 1);
    Value cols = b.create<memref::DimOp>(loc, rhs, 1);

    auto blocks = b.create<scf::ParallelOp>(loc, ValueRange{zero, zero}, ValueRange{rows, cols},
                                            ValueRange{constant(kBlockRows), constant(kBlockCols)});
    b.setInsertionPointToStart(blocks.getBody());
    Value rowBase = blocks.getInductionVars()[0];
    Value colBase = blocks.getInductionVars()[1];
    Value rowEnd = minimum(addConstant(rowBase, kBlockRows), rows);
    Value colEnd = minimum(addConstant(colBase, kBlockCols), cols);
    Value rowTilesEnd = fullStepsEnd(rowBase, rowEnd, kTileRows);
    Value colTilesEnd = fullStepsEnd(colBase, colEnd, tileCols);

    Value k0 = loop(zero, depth, kBlockDepth).getInductionVar();
    Value kEnd = minimum(addConstant(k0, kBlockDepth), depth);
    {
        OpBuilder::InsertionGuard rowGuard(b);
        Value i = loop(rowBase, rowTilesEnd, kTileRows).getInductionVar();
        {
            OpBuilder::InsertionGuard colGuard(b);
            Value j = loop(colBase, colTilesEnd, tileCols).getInductionVar();

            // Yazmaçlardaki parçanın satırları ve vektörlerinin başladığı sütunlar.
            llvm::SmallVector<Value, kTileRows> tileRows;
            llvm::SmallVector<Value, kTileVectors> tileColumns;
            for (int64_t r = 0; r < kTileRows; ++r) tileRows.push_back(addConstant(i, r));
            for (int64_t v = 0; v < kTileVectors; ++v)
                tileColumns.push_back(addConstant(j, v * width));

            llvm::SmallVector<Value, kTileRows * kTileVectors> initial;
            for (Value row : tileRows)
                for (Value col : tileColumns)
                    initial.push_back(
                        b.create<vector::LoadOp>(loc, vectorType, out, ValueRange{row, col}));

            auto depthLoop = loop(k0, kEnd, 1, initial);
            Value k = depthLoop.getInductionVar();
            llvm::SmallVector<Value, kTileVectors> rhsVectors;
            for (Value col : tileColumns)
                rhsVectors.push_back(b.create<vector::LoadOp>(loc, vectorType, rhs, ValueRange{k, col}));
            llvm::SmallVector<Value, kTileRows * kTileVectors> sums;
            for (int64_t r = 0; r < kTileRows; ++r) {
                Value a = b.create<memref::LoadOp>(loc, lhs, ValueRange{tileRows[r], k});
                Value spread = b.create<vector::BroadcastOp>(loc, vectorType, a);
                for (int64_t v = 0; v < kTileVectors; ++v)
                    sums.push_back(multiplyAdd(depthLoop.getRegionIterArgs()[r * kTileVectors + v],
                                               spread, rhsVectors[v]));
            }
            b.create<scf::YieldOp>(loc, sums);

            b.setInsertionPointAfter(depthLoop);
            for (int64_t r = 0; r < kTileRows; ++r)
                for (int64_t v = 0; v < kTileVectors; ++v)
                    b.create<vector::StoreOp>(loc, depthLoop.getResult(r * kTileVectors + v), out,
                                              ValueRange{tileRows[r], tileColumns[v]});
        }
        plainPart(i, addConstant(i, kTileRows), colTilesEnd, colEnd, k0, kEnd);
    }
    plainPart(rowTilesEnd, rowEnd, colBase, colEnd, k0, kEnd);
}

class ParallelMatmulPass : public PassWrapper<ParallelMatmulPass, OperationPass<ModuleOp>> {
public:
    MLIR_DEFINE_EXPLICIT_INTERNAL_INLINE_TYPE_ID(ParallelMatmulPass)

    StringRef getArgument() const final { return "marlin-parallel-matmul"; }
    StringRef getDescription() const final {
        return "CPU fonksiyonlarindaki linalg.matmul islemlerini dosemeli paralel donguye acar";
    }
    void getDependentDialects(DialectRegistry& registry) const override {
        registry.insert<scf::SCFDialect, memref::MemRefDialect, affine::AffineDialect,
                        arith::ArithDialect, vector::VectorDialect>();
    }

    void runOnOperation() override {
        // func.func ve görevlerden çağrılan coroutine fonksiyonlar (async.func).
        for (FunctionOpInterface function : getOperation().getOps<FunctionOpInterface>()) {
            if (function.isExternal() || function->hasAttr(kGpuFunctionAttr)) continue;

            llvm::SmallVector<linalg::MatmulOp, 4> products;
            function.walk([&](linalg::MatmulOp op) { products.push_back(op); });

            IRRewriter rewriter(function.getContext());
            for (linalg::MatmulOp op : products) {
                // Döşemeli çekirdek düz (satır satır, bitişik) matrisler içindir;
                // başka bir yerleşimde basit paralel döngüye açılır.
                const bool plain = op.hasPureBufferSemantics() &&
                                   llvm::all_of(op->getOperands(), isPlainMatrix);
                if (plain) {
                    buildTiledMatmul(op);
                } else {
                    rewriter.setInsertionPoint(op);
                    if (failed(linalg::linalgOpToParallelLoops(rewriter, op))) {
                        op.emitError("matris carpimi paralel donguye acilamadi");
                        return signalPassFailure();
                    }
                }
                rewriter.eraseOp(op);
            }
        }
    }
};

} // namespace

std::unique_ptr<Pass> marlin::createParallelMatmulPass() {
    return std::make_unique<ParallelMatmulPass>();
}
