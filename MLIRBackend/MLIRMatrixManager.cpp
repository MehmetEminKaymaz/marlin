#include "MLIRMatrixManager.h"

#include "llvm/ADT/SmallVector.h"
#include "mlir/Dialect/Arith/IR/Arith.h"
#include "mlir/Dialect/Bufferization/IR/Bufferization.h"
#include "mlir/Dialect/Linalg/IR/Linalg.h"
#include "mlir/Dialect/Tensor/IR/Tensor.h"
#include "mlir/Dialect/Utils/StructuredOpsUtils.h"
#include "mlir/IR/AffineMap.h"

using namespace mlir;

// Boyut sayısı `rank`, bütün boyutları çalışırken belli olan şekil.
static llvm::SmallVector<int64_t, 4> dynamicShape(unsigned rank) {
    return llvm::SmallVector<int64_t, 4>(rank, ShapedType::kDynamic);
}

static MemRefType bufferTypeOf(mlir::Type elementType, unsigned rank) {
    return MemRefType::get(dynamicShape(rank), elementType);
}

static RankedTensorType tensorTypeOf(mlir::Type elementType, unsigned rank) {
    return RankedTensorType::get(dynamicShape(rank), elementType);
}

static mlir::Type elementTypeOf(Value matrix) {
    return cast<ShapedType>(matrix.getType()).getElementType();
}

static unsigned rankOf(Value matrix) {
    return static_cast<unsigned>(cast<ShapedType>(matrix.getType()).getRank());
}

// ===========================================================================
// Köprüler ve yardımcılar
// ===========================================================================

// `restrict`: bu tensor başka hiçbir tensor ile aynı belleği göstermez.
// Matrisler değer olduğu için bu her zaman doğrudur ve bufferization'ın
// analiz yapabilmesi için gereklidir. `writable` yalnızca yeni ayrılmış sonuç
// belleklerinde verilir; değişkenlerden okunan matrisler salt okunurdur.
Value MLIRMatrixManager::toTensor(Value buffer, bool writable, Location loc) {
    return ctx.builder.create<bufferization::ToTensorOp>(
        loc, tensorTypeOf(elementTypeOf(buffer), rankOf(buffer)), buffer,
        /*restrict=*/true, writable);
}

// `read_only`: dönen bellek bu noktada yalnızca okunacak (ya da sahibi
// değişecek); bufferization korumak için kopya çıkarmasın.
Value MLIRMatrixManager::toBuffer(Value tensor, Location loc) {
    return ctx.builder.create<bufferization::ToMemrefOp>(
        loc, bufferTypeOf(elementTypeOf(tensor), rankOf(tensor)), tensor, /*read_only=*/true);
}

Value MLIRMatrixManager::dim(Value tensor, unsigned dimension, Location loc) {
    return ctx.builder.create<tensor::DimOp>(loc, tensor, static_cast<int64_t>(dimension));
}

llvm::SmallVector<Value, 4> MLIRMatrixManager::shapeOf(Value tensor, Location loc) {
    llvm::SmallVector<Value, 4> sizes;
    for (unsigned d = 0; d < rankOf(tensor); ++d)
        sizes.push_back(dim(tensor, d, loc));
    return sizes;
}

Value MLIRMatrixManager::zeroOf(mlir::Type elementType, Location loc) {
    return ctx.builder.create<arith::ConstantOp>(
        loc, cast<TypedAttr>(ctx.builder.getZeroAttr(elementType)));
}

Value MLIRMatrixManager::allocateBuffer(llvm::ArrayRef<Value> sizes, mlir::Type elementType,
                                        Location loc) {
    return ctx.builder.create<memref::AllocOp>(
        loc, bufferTypeOf(elementType, static_cast<unsigned>(sizes.size())), ValueRange(sizes));
}

MLIRMatrixManager::Result MLIRMatrixManager::allocate(llvm::ArrayRef<Value> sizes,
                                                      mlir::Type elementType, Location loc) {
    Value buffer = allocateBuffer(sizes, elementType, loc);
    return {toTensor(buffer, /*writable=*/true, loc), buffer};
}

// Boyut sayısı değişken olduğu için hata mesajındaki sayılar (şekiller,
// indeksler) çalışma zamanına tek tek bildirilir; ardından çağrılan hata
// fonksiyonu onları biçimlendirip yazar.
void MLIRMatrixManager::report(int slot, llvm::ArrayRef<Value> indexValues, Location loc) {
    OpBuilder& b = ctx.builder;
    Value slotValue = b.create<arith::ConstantIntOp>(loc, slot, 64);
    for (Value value : indexValues) {
        Value asI64 = b.create<arith::IndexCastOp>(loc, b.getI64Type(), value);
        runtime.call("minik_report_push", {slotValue, asI64}, loc);
    }
}

// Bütün girdiler ilk girdiyle aynı şekilde olmalı (her boyutta aynı uzunluk).
void MLIRMatrixManager::checkSameShape(llvm::ArrayRef<Value> inputs, int line, Location loc) {
    OpBuilder& b = ctx.builder;
    if (inputs.size() < 2) return;

    auto shape = shapeOf(inputs[0], loc);
    for (Value other : inputs.drop_front()) {
        auto otherShape = shapeOf(other, loc);

        Value mismatch;
        for (size_t d = 0; d < shape.size(); ++d) {
            Value differs = b.create<arith::CmpIOp>(loc, arith::CmpIPredicate::ne,
                                                    shape[d], otherShape[d]);
            mismatch = mismatch ? b.create<arith::OrIOp>(loc, mismatch, differs).getResult()
                                : differs;
        }
        runtime.ifThen(mismatch, loc, [&] {
            report(0, shape, loc);
            report(1, otherShape, loc);
            runtime.call("minik_panic_shape", {b.create<arith::ConstantIntOp>(loc, line, 32)}, loc);
        });
    }
}

// ===========================================================================
// Oluşturma
// ===========================================================================

MLIRMatrixManager::Result MLIRMatrixManager::filled(llvm::ArrayRef<Value> sizes, Value value,
                                                    Location loc) {
    OpBuilder& b = ctx.builder;
    Result out = allocate(sizes, value.getType(), loc);

    mlir::Type resultTypes[] = {out.tensor.getType()};
    auto fill = b.create<linalg::FillOp>(loc, TypeRange(resultTypes), ValueRange(value),
                                         ValueRange(out.tensor));
    return {fill->getResult(0), out.buffer};
}

// ===========================================================================
// Eleman eleman işlemler
// ===========================================================================

//   %r = linalg.generic  ins(%a, %b, ... : tensor) outs(%yeni : tensor) {
//          ^(%ea, %eb, ..., %cikti):  yield body(%ea, %eb, ...)
//        }
//
// Bütün indeks eşlemeleri özdeşliktir ve bütün boyutlar "parallel"dir: her
// elemanın hesabı diğerlerinden bağımsızdır. Bu bilgi IR'de açıkça durduğu
// için işlem ileride paralel döngüye ya da GPU çekirdeğine çevrilebilir.
MLIRMatrixManager::Result MLIRMatrixManager::elementwise(llvm::ArrayRef<Value> inputs,
                                                         mlir::Type resultElementType, int line,
                                                         ElementFn body, Location loc) {
    OpBuilder& b = ctx.builder;
    const unsigned rank = rankOf(inputs[0]);
    checkSameShape(inputs, line, loc);

    Result out = allocate(shapeOf(inputs[0], loc), resultElementType, loc);

    llvm::SmallVector<AffineMap, 4> maps(inputs.size() + 1, b.getMultiDimIdentityMap(rank));
    llvm::SmallVector<utils::IteratorType, 4> iterators(rank, utils::IteratorType::parallel);
    mlir::Type resultTypes[] = {out.tensor.getType()};

    auto generic = b.create<linalg::GenericOp>(
        loc, TypeRange(resultTypes), ValueRange(inputs), ValueRange(out.tensor), maps, iterators,
        [&](OpBuilder& nested, Location nestedLoc, ValueRange args) {
            // Son argüman çıktının eski değeridir; kullanılmaz.
            nested.create<linalg::YieldOp>(nestedLoc, body(args.drop_back()));
        });
    return {generic.getResult(0), out.buffer};
}

// Toplam: çıktı sıfır boyutlu bir tensor'dur (tek değer) ve bütün boyutlar
// "reduction"dır. Biriktirici yığıtta durur; ara matris ayrılmaz.
Value MLIRMatrixManager::sum(llvm::ArrayRef<Value> inputs, mlir::Type elementType, int line,
                             ElementFn body, Location loc) {
    OpBuilder& b = ctx.builder;
    const unsigned rank = rankOf(inputs[0]);
    checkSameShape(inputs, line, loc);

    Value accumulator = ctx.createEntryAlloca(loc, MemRefType::get({}, elementType));
    b.create<memref::StoreOp>(loc, zeroOf(elementType, loc), accumulator);
    Value accumulatorTensor = b.create<bufferization::ToTensorOp>(
        loc, RankedTensorType::get({}, elementType), accumulator,
        /*restrict=*/true, /*writable=*/true);

    llvm::SmallVector<AffineMap, 4> maps(inputs.size(), b.getMultiDimIdentityMap(rank));
    maps.push_back(AffineMap::get(/*dimCount=*/rank, /*symbolCount=*/0, &ctx.context)); // (...) -> ()
    llvm::SmallVector<utils::IteratorType, 4> iterators(rank, utils::IteratorType::reduction);
    mlir::Type resultTypes[] = {accumulatorTensor.getType()};

    const bool isFloat = isa<FloatType>(elementType);
    auto generic = b.create<linalg::GenericOp>(
        loc, TypeRange(resultTypes), ValueRange(inputs), ValueRange(accumulatorTensor), maps,
        iterators,
        [&](OpBuilder& nested, Location nestedLoc, ValueRange args) {
            Value element = body(args.drop_back());
            Value total = isFloat
                ? nested.create<arith::AddFOp>(nestedLoc, args.back(), element).getResult()
                : nested.create<arith::AddIOp>(nestedLoc, args.back(), element).getResult();
            nested.create<linalg::YieldOp>(nestedLoc, total);
        });
    return b.create<tensor::ExtractOp>(loc, generic.getResult(0), ValueRange());
}

// ===========================================================================
// Matris çarpımı ve devrik (iki boyut)
// ===========================================================================

// linalg.matmul sonucu çıktının üzerine EKLER; bu yüzden çıktı önce
// sıfırlanır. İşlemin "matris çarpımı" olduğu IR'de adıyla durur.
MLIRMatrixManager::Result MLIRMatrixManager::matmul(Value lhs, Value rhs, int line, Location loc) {
    OpBuilder& b = ctx.builder;

    // (r x k) * (k x c): soldakinin sütun sayısı sağdakinin satır sayısına eşit olmalı.
    auto lhsShape = shapeOf(lhs, loc);
    auto rhsShape = shapeOf(rhs, loc);
    Value mismatch = b.create<arith::CmpIOp>(loc, arith::CmpIPredicate::ne,
                                             lhsShape[1], rhsShape[0]);
    runtime.ifThen(mismatch, loc, [&] {
        report(0, lhsShape, loc);
        report(1, rhsShape, loc);
        runtime.call("minik_panic_matmul", {b.create<arith::ConstantIntOp>(loc, line, 32)}, loc);
    });

    mlir::Type elementType = elementTypeOf(lhs);
    Value sizes[] = {lhsShape[0], rhsShape[1]};
    Result out = filled(sizes, zeroOf(elementType, loc), loc);

    Value inputs[] = {lhs, rhs};
    mlir::Type resultTypes[] = {out.tensor.getType()};
    auto product = b.create<linalg::MatmulOp>(loc, TypeRange(resultTypes), ValueRange(inputs),
                                              ValueRange(out.tensor));
    return {product->getResult(0), out.buffer};
}

//   çıktı[i, j] = girdi[j, i]
MLIRMatrixManager::Result MLIRMatrixManager::transpose(Value input, Location loc) {
    OpBuilder& b = ctx.builder;
    Value sizes[] = {dim(input, 1, loc), dim(input, 0, loc)};
    Result out = allocate(sizes, elementTypeOf(input), loc);

    const unsigned swapped[] = {1, 0};
    AffineMap maps[] = {AffineMap::getPermutationMap(llvm::ArrayRef<unsigned>(swapped), &ctx.context),
                        b.getMultiDimIdentityMap(2)};
    llvm::SmallVector<utils::IteratorType, 2> iterators(2, utils::IteratorType::parallel);
    mlir::Type resultTypes[] = {out.tensor.getType()};

    auto generic = b.create<linalg::GenericOp>(
        loc, TypeRange(resultTypes), ValueRange(input), ValueRange(out.tensor), maps, iterators,
        [](OpBuilder& nested, Location nestedLoc, ValueRange args) {
            nested.create<linalg::YieldOp>(nestedLoc, args[0]);
        });
    return {generic.getResult(0), out.buffer};
}

// ===========================================================================
// Eleman erişimi
// ===========================================================================

// Her boyutta indeks < uzunluk olmalı. İşaretsiz karşılaştırılır; negatif
// indeks de böylece yakalanır.
void MLIRMatrixManager::checkIndex(Value buffer, llvm::ArrayRef<Value> indices, int line,
                                   Location loc) {
    OpBuilder& b = ctx.builder;

    llvm::SmallVector<Value, 4> sizes;
    Value outOfBounds;
    for (size_t d = 0; d < indices.size(); ++d) {
        sizes.push_back(b.create<memref::DimOp>(loc, buffer, static_cast<int64_t>(d)));
        Value out = b.create<arith::CmpIOp>(loc, arith::CmpIPredicate::uge, indices[d], sizes[d]);
        outOfBounds = outOfBounds ? b.create<arith::OrIOp>(loc, outOfBounds, out).getResult() : out;
    }
    runtime.ifThen(outOfBounds, loc, [&] {
        report(0, indices, loc);
        report(1, sizes, loc);
        runtime.call("minik_panic_matrix_index",
                     {b.create<arith::ConstantIntOp>(loc, line, 32)}, loc);
    });
}

Value MLIRMatrixManager::loadElement(Value buffer, llvm::ArrayRef<Value> indices, int line,
                                     Location loc) {
    checkIndex(buffer, indices, line, loc);
    return ctx.builder.create<memref::LoadOp>(loc, buffer, ValueRange(indices));
}

void MLIRMatrixManager::storeElement(Value buffer, llvm::ArrayRef<Value> indices, Value value,
                                     int line, Location loc) {
    checkIndex(buffer, indices, line, loc);
    ctx.builder.create<memref::StoreOp>(loc, value, buffer, ValueRange(indices));
}
