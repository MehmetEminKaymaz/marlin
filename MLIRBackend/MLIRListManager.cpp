#include "MLIRListManager.h"

#include "llvm/Support/MathExtras.h"
#include "mlir/Dialect/Arith/IR/Arith.h"
#include "mlir/Dialect/SCF/IR/SCF.h"
#include <algorithm>

using namespace mlir;

static const int64_t kMinCapacity = 4;

// ===========================================================================
// Yardımcılar
// ===========================================================================

Value MLIRListManager::loadData(Value list, Location loc) {
    return ctx.builder.create<memref::LoadOp>(loc, list);
}

Value MLIRListManager::resized(Value data, Value newLength, Location loc) {
    OpBuilder& b = ctx.builder;
    OpFoldResult sizes[] = {newLength};
    OpFoldResult strides[] = {b.getIndexAttr(1)};
    return b.create<memref::ReinterpretCastOp>(
        loc, cast<MemRefType>(data.getType()), data,
        /*offset=*/OpFoldResult(b.getIndexAttr(0)), sizes, strides);
}

void MLIRListManager::emitIf(Value cond, Location loc, llvm::function_ref<void()> body) {
    OpBuilder& b = ctx.builder;
    auto ifOp = b.create<scf::IfOp>(loc, TypeRange(), cond,
                                    /*addThenBlock=*/true, /*addElseBlock=*/false);
    OpBuilder::InsertionGuard guard(b);
    b.setInsertionPointToEnd(&ifOp.getThenRegion().front());
    body();
    b.create<scf::YieldOp>(loc);
}

Value MLIRListManager::toI64(Value index, Location loc) {
    return ctx.builder.create<arith::IndexCastOp>(loc, ctx.builder.getI64Type(), index);
}

// İndeks işaretsiz karşılaştırılır: negatif bir indeks çok büyük bir sayı
// olarak görünür ve o da yakalanır.
void MLIRListManager::checkIndex(Value index, Value length, int line, Location loc) {
    OpBuilder& b = ctx.builder;
    Value outOfBounds = b.create<arith::CmpIOp>(loc, arith::CmpIPredicate::uge, index, length);
    emitIf(outOfBounds, loc, [&] {
        Value lineValue = b.create<arith::ConstantIntOp>(loc, line, 32);
        runtime.call("minik_panic_index", {toI64(index, loc), toI64(length, loc), lineValue}, loc);
    });
}

// ===========================================================================
// Oluşturma ve sorgulama
// ===========================================================================

Value MLIRListManager::create(mlir::Type elementType, llvm::ArrayRef<Value> elements,
                              Location loc) {
    OpBuilder& b = ctx.builder;
    MemRefType dataType = MemRefType::get({ShapedType::kDynamic}, elementType);
    MemRefType cellType = MemRefType::get({}, dataType);

    const int64_t count = static_cast<int64_t>(elements.size());
    const int64_t capacity =
        std::max<int64_t>(kMinCapacity, static_cast<int64_t>(llvm::PowerOf2Ceil(count)));

    Value cell = b.create<memref::AllocOp>(loc, cellType);
    Value capacityValue = b.create<arith::ConstantIndexOp>(loc, capacity);
    Value buffer = b.create<memref::AllocOp>(loc, dataType, ValueRange(capacityValue));
    Value data = resized(buffer, b.create<arith::ConstantIndexOp>(loc, count), loc);

    for (int64_t i = 0; i < count; ++i) {
        Value index = b.create<arith::ConstantIndexOp>(loc, i);
        b.create<memref::StoreOp>(loc, elements[i], data, ValueRange(index));
    }
    b.create<memref::StoreOp>(loc, data, cell);
    return cell;
}

Value MLIRListManager::length(Value list, Location loc) {
    return ctx.builder.create<memref::DimOp>(loc, loadData(list, loc), 0);
}

Value MLIRListManager::loadElement(Value list, Value index, int line, Location loc) {
    OpBuilder& b = ctx.builder;
    Value data = loadData(list, loc);
    checkIndex(index, b.create<memref::DimOp>(loc, data, 0), line, loc);
    return b.create<memref::LoadOp>(loc, data, ValueRange(index));
}

void MLIRListManager::storeElement(Value list, Value index, Value value, int line,
                                   Location loc) {
    OpBuilder& b = ctx.builder;
    Value data = loadData(list, loc);
    checkIndex(index, b.create<memref::DimOp>(loc, data, 0), line, loc);
    b.create<memref::StoreOp>(loc, value, data, ValueRange(index));
}

// ===========================================================================
// push / pop / free
// ===========================================================================

void MLIRListManager::push(Value list, Value value, Location loc) {
    OpBuilder& b = ctx.builder;
    Value data = loadData(list, loc);
    Value length = b.create<memref::DimOp>(loc, data, 0);

    Value zero = b.create<arith::ConstantIndexOp>(loc, 0);
    Value one = b.create<arith::ConstantIndexOp>(loc, 1);
    Value minCapacity = b.create<arith::ConstantIndexOp>(loc, kMinCapacity);

    // Dolu mu?  uzunluk >= 4  ve  uzunluk ikinin kuvveti  (n & (n-1) == 0)
    Value lengthMinusOne = b.create<arith::SubIOp>(loc, length, one);
    Value lowBits = b.create<arith::AndIOp>(loc, length, lengthMinusOne);
    Value isPowerOfTwo = b.create<arith::CmpIOp>(loc, arith::CmpIPredicate::eq, lowBits, zero);
    Value atLeastMin = b.create<arith::CmpIOp>(loc, arith::CmpIPredicate::uge, length, minCapacity);
    Value isFull = b.create<arith::AndIOp>(loc, isPowerOfTwo, atLeastMin);

    // Doluysa belleği iki katına çıkar. realloc eski elemanları taşır ve eski
    // belleği bırakır; yeni veri hücreye yazıldığı için listeyi gösteren
    // herkes onu görür.
    emitIf(isFull, loc, [&] {
        Value two = b.create<arith::ConstantIndexOp>(loc, 2);
        Value newCapacity = b.create<arith::MulIOp>(loc, length, two);
        Value grown = b.create<memref::ReallocOp>(
            loc, cast<MemRefType>(data.getType()), data, newCapacity);
        b.create<memref::StoreOp>(loc, grown, list);
    });

    // Uzunluğu bir artır ve yeni elemanı sona yaz.
    Value current = loadData(list, loc);
    Value newLength = b.create<arith::AddIOp>(loc, length, one);
    Value extended = resized(current, newLength, loc);
    b.create<memref::StoreOp>(loc, value, extended, ValueRange(length));
    b.create<memref::StoreOp>(loc, extended, list);
}

Value MLIRListManager::pop(Value list, int line, Location loc) {
    OpBuilder& b = ctx.builder;
    Value data = loadData(list, loc);
    Value length = b.create<memref::DimOp>(loc, data, 0);

    Value zero = b.create<arith::ConstantIndexOp>(loc, 0);
    Value isEmpty = b.create<arith::CmpIOp>(loc, arith::CmpIPredicate::eq, length, zero);
    emitIf(isEmpty, loc, [&] {
        Value lineValue = b.create<arith::ConstantIntOp>(loc, line, 32);
        runtime.call("minik_panic_pop", {lineValue}, loc);
    });

    // Bellek küçültülmez; yalnızca uzunluk bir azalır.
    Value one = b.create<arith::ConstantIndexOp>(loc, 1);
    Value last = b.create<arith::SubIOp>(loc, length, one);
    Value value = b.create<memref::LoadOp>(loc, data, ValueRange(last));
    b.create<memref::StoreOp>(loc, resized(data, last, loc), list);
    return value;
}

void MLIRListManager::free(Value list, Location loc) {
    OpBuilder& b = ctx.builder;
    b.create<memref::DeallocOp>(loc, loadData(list, loc));
    b.create<memref::DeallocOp>(loc, list);
}
