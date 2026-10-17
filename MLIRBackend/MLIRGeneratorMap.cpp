// MLIRGenerator — map<K, V>
//
// Map tek bir değer değildir; yapraklarına ayrılır (bkz. SemType.h, MAP):
//
//   [0] tablo    i64: anahtar → satır tablosu (runtime/marlin_map.c)
//   [1] keys     list<K>: anahtarlar, eklenme sırasıyla
//   [2..] values list<V>'nin yaprakları (V struct ise alan başına bir liste)
//
// Satır r'deki anahtar keys[r], değeri values[r]'dir. Buradaki her işlem
// tabloya sorulan bir satır numarası ve listeler üzerinde sıradan liste
// işlemleridir:
//
//   m[k]          r = find(k) (yoksa program durur); values[r]
//   m[k] = v      r = find(k); yoksa keys ve values'a eklenir, tabloya
//                 yazılır; varsa values[r] değişir
//   m[k].x = v    r = find(k) (yoksa program durur); x sütununa yazılır
//   has(m, k)     find(k) >= 0
//   remove(m, k)  satır listelerden silinir (sonrakiler kayar, sıra korunur);
//                 tablo da sonraki satır numaralarını bir azaltır
//   for k in m    keys üzerinde döngü (MLIRGeneratorList.cpp)
//   free(m)       tablo ve bütün listeler bırakılır
//
// Değerlerin belleği (matris, dizi, string) listelerde olduğu gibi yönetilir
// (adoptElement / releaseElement).

#include "MLIRGenerator.h"

#include "mlir/Dialect/Arith/IR/Arith.h"
#include "mlir/Dialect/MemRef/IR/MemRef.h"
#include "mlir/Dialect/SCF/IR/SCF.h"

namespace {

// Hata mesajında anahtarın nasıl yazılacağı (bkz. marlin_panic_map_key).
int32_t keyKind(const SemType& keyType) {
    if (keyType.isString()) return 4;
    if (keyType.isBool()) return 3;
    if (keyType.isChar()) return 2;
    return keyType.isSigned ? 0 : 1;
}

// map'in değer sütunlarının tipleri (list<V>'nin yapraklarının eleman tipleri)
std::vector<SemType> valueColumnTypes(const SemType& mapType) {
    std::vector<SemType> types;
    for (const SemLeaf& leaf : leavesOf(SemType::List(mapType.valueType())))
        types.push_back(leaf.type.elementType());
    return types;
}

} // namespace

// ===========================================================================
// Oluşturma ve arama
// ===========================================================================

MLIRGenerator::Values MLIRGenerator::emitMapLiteral(const SemType& mapType, mlir::Location loc) {
    Values parts;
    parts.push_back(runtime.callValue("marlin_map_new", {}, ctx.builder.getI64Type(), loc));
    parts.push_back(listManager.create(typeManager.convert(mapType.keyType()), {}, loc));
    for (const SemType& type : valueColumnTypes(mapType))
        parts.push_back(listManager.create(typeManager.convert(type), {}, loc));
    return parts;
}

// Tamsayı, char ve bool 64 bite genişletilir (işaretliler işaretiyle); string
// anahtar zaten stringin adresidir (i64).
mlir::Value MLIRGenerator::emitMapKey(Expression* key, const SemType& keyType) {
    return toMapKey(emitExpr(key), keyType, locOf(key));
}

mlir::Value MLIRGenerator::toMapKey(mlir::Value value, const SemType& keyType, mlir::Location loc) {
    if (keyType.isString() || (keyType.isInt() && keyType.width == 64)) return value;
    mlir::Type i64 = ctx.builder.getI64Type();
    if (keyType.isInt() && keyType.isSigned)
        return ctx.builder.create<mlir::arith::ExtSIOp>(loc, i64, value);
    return ctx.builder.create<mlir::arith::ExtUIOp>(loc, i64, value);
}

mlir::Value MLIRGenerator::mapFind(mlir::Value table, mlir::Value key, const SemType& keyType,
                                   mlir::Location loc) {
    mlir::Value isString = arithManager.buildIntConstant(keyType.isString(), ctx.builder.getI32Type(), loc);
    return runtime.callValue("marlin_map_find", {table, key, isString}, ctx.builder.getI64Type(), loc);
}

mlir::Value MLIRGenerator::mapRow(mlir::Value table, mlir::Value key, const SemType& keyType,
                                  int line, mlir::Location loc) {
    mlir::OpBuilder& b = ctx.builder;
    mlir::Value row = mapFind(table, key, keyType, loc);
    mlir::Value zero = arithManager.buildIntConstant(0, b.getI64Type(), loc);
    mlir::Value missing = b.create<mlir::arith::CmpIOp>(loc, mlir::arith::CmpIPredicate::slt, row, zero);
    runtime.ifThen(missing, loc, [&] {
        runtime.call("marlin_panic_map_key",
                     {key, arithManager.buildIntConstant(keyKind(keyType), b.getI32Type(), loc),
                      arithManager.buildIntConstant(line, b.getI32Type(), loc)},
                     loc);
    });
    return b.create<mlir::arith::IndexCastOp>(loc, b.getIndexType(), row);
}

// ===========================================================================
// Okuma ve yazma
// ===========================================================================

MLIRGenerator::Values MLIRGenerator::emitMapElement(ArrayAccessExpr* node, size_t begin, size_t count) {
    auto loc = locOf(node);
    const SemType& mapType = node->target->type;
    const std::vector<SemType> types = valueColumnTypes(mapType);

    Values parts = emitLeaves(node->target.get());
    mlir::Value key = emitMapKey(node->indices[0].get(), mapType.keyType());
    mlir::Value row = mapRow(parts[kMapIndexLeafPos], key, mapType.keyType(), node->line, loc);

    Values values;
    for (size_t i = begin; i < begin + count; ++i) {
        mlir::Value stored = listManager.loadElement(parts[kMapValuesLeafPos + i], row, node->line, loc,
                                                     /*checked=*/false);
        values.push_back(borrowElement(stored, types[i], loc));
    }
    return values;
}

// `access` hedefteki m[k]; `begin`: hedef bir alansa (m[k].x) değerin
// yaprakları içinde başladığı yer.
void MLIRGenerator::assignMapElement(AssignmentStmt* node, ArrayAccessExpr* access, size_t begin) {
    auto loc = locOf(node);
    mlir::OpBuilder& b = ctx.builder;
    const SemType& mapType = access->target->type;
    const SemType& keyType = mapType.keyType();
    const std::vector<SemType> allTypes = valueColumnTypes(mapType);
    const size_t count = leavesOf(node->target->type).size();
    const std::vector<SemType> types(allTypes.begin() + begin, allTypes.begin() + begin + count);

    Values parts = emitLeaves(access->target.get());
    mlir::Value table = parts[kMapIndexLeafPos];
    // Anahtarın kendisi (keys listesine) ve tablodaki gösterimi (i64)
    mlir::Value keyValue = emitExpr(access->indices[0].get());
    mlir::Value key = toMapKey(keyValue, keyType, loc);
    Values values = emitLeaves(node->value.get());
    Values cells(parts.begin() + kMapValuesLeafPos + begin,
                 parts.begin() + kMapValuesLeafPos + begin + count);

    // m[k].x = v: anahtar olmalı; elemanın bir alanı yerinde değişir.
    if (access != node->target.get()) {
        mlir::Value row = mapRow(table, key, keyType, node->line, loc);
        adoptOwnedValues(values, types, node->line, loc);
        writeElement(cells, types, row, values, node->line, loc, /*checked=*/false);
        flushTemporaries(loc);
        return;
    }

    // m[k] = v: değerler ve string anahtar dallanmadan ÖNCE sahiplenilir. (Bir
    // geçicinin devralınması derlerken kaydedilir; iki dalda farklı yapılamaz.)
    // Anahtar zaten varsa sahiplenilen anahtar bırakılır.
    adoptOwnedValues(values, types, node->line, loc);
    mlir::Value ownedKey = keyType.isString() ? ownHandle(keyValue, keyType, loc) : keyValue;

    mlir::Value row = mapFind(table, key, keyType, loc);
    mlir::Value zero = arithManager.buildIntConstant(0, b.getI64Type(), loc);
    mlir::Value isNew = b.create<mlir::arith::CmpIOp>(loc, mlir::arith::CmpIPredicate::slt, row, zero);
    auto branch = b.create<mlir::scf::IfOp>(loc, mlir::TypeRange(), isNew,
                                            /*addThenBlock=*/true, /*addElseBlock=*/true);
    {
        // Yeni anahtar: listelerin sonuna eklenir, tabloya son satır olarak yazılır.
        mlir::OpBuilder::InsertionGuard guard(b);
        b.setInsertionPointToEnd(&branch.getThenRegion().front());
        mlir::Value newRow = b.create<mlir::arith::IndexCastOp>(
            loc, b.getI64Type(), listManager.length(parts[kMapKeysLeafPos], loc));
        listManager.push(parts[kMapKeysLeafPos], ownedKey, loc);
        runtime.call("marlin_map_insert",
                     {table, key, arithManager.buildIntConstant(keyType.isString(), b.getI32Type(), loc),
                      newRow},
                     loc);
        for (size_t i = 0; i < count; ++i) {
            // (Dizi: elemana ait yeni bir kopya; matris ve handle yukarıda sahiplenildi.)
            mlir::Value value = types[i].isArray() ? adoptElement(values[i], types[i], node->line, loc)
                                                   : values[i];
            listManager.push(cells[i], value, loc);
        }
        b.create<mlir::scf::YieldOp>(loc);
    }
    {
        // Var olan anahtar: değeri değişir.
        mlir::OpBuilder::InsertionGuard guard(b);
        b.setInsertionPointToEnd(&branch.getElseRegion().front());
        if (keyType.isString()) releaseHandle(ownedKey, keyType, loc);
        mlir::Value index = b.create<mlir::arith::IndexCastOp>(loc, b.getIndexType(), row);
        writeElement(cells, types, index, values, node->line, loc, /*checked=*/false);
        b.create<mlir::scf::YieldOp>(loc);
    }
    flushTemporaries(loc);
}

// ===========================================================================
// len / free / has / remove / keys
// ===========================================================================

void MLIRGenerator::emitMapBuiltin(FunctionCallExpr* node) {
    auto loc = locOf(node);
    mlir::OpBuilder& b = ctx.builder;
    const SemType& mapType = node->arguments[0]->type;
    const SemType& keyType = mapType.keyType();
    const std::vector<SemType> types = valueColumnTypes(mapType);

    Values parts = emitLeaves(node->arguments[0].get());
    mlir::Value table = parts[kMapIndexLeafPos];
    mlir::Value keys = parts[kMapKeysLeafPos];
    lastValue = mlir::Value{};

    switch (node->builtin) {
        case Builtin::Len:
            lastValue = b.create<mlir::arith::IndexCastOp>(loc, b.getI64Type(), listManager.length(keys, loc));
            return;

        case Builtin::Free:
            runtime.call("marlin_map_free", {table}, loc);
            freeListValue(keys, SemType::List(keyType), loc);
            for (size_t i = 0; i < types.size(); ++i)
                freeListValue(parts[kMapValuesLeafPos + i], SemType::List(types[i]), loc);
            return;

        case Builtin::Has: {
            mlir::Value row = mapFind(table, emitMapKey(node->arguments[1].get(), keyType), keyType, loc);
            mlir::Value zero = arithManager.buildIntConstant(0, b.getI64Type(), loc);
            lastValue = b.create<mlir::arith::CmpIOp>(loc, mlir::arith::CmpIPredicate::sge, row, zero);
            return;
        }

        // Satır bütün listelerden silinir; sonraki satırlar bir öne kayar.
        case Builtin::Remove: {
            mlir::Value key = emitMapKey(node->arguments[1].get(), keyType);
            mlir::Value row = mapFind(table, key, keyType, loc);
            mlir::Value zero = arithManager.buildIntConstant(0, b.getI64Type(), loc);
            mlir::Value found = b.create<mlir::arith::CmpIOp>(loc, mlir::arith::CmpIPredicate::sge, row, zero);
            runtime.ifThen(found, loc, [&] {
                mlir::Value index = b.create<mlir::arith::IndexCastOp>(loc, b.getIndexType(), row);
                runtime.call("marlin_map_remove",
                             {table, key, arithManager.buildIntConstant(keyType.isString(), b.getI32Type(), loc)},
                             loc);
                releaseElement(listManager.loadElement(keys, index, node->line, loc, /*checked=*/false),
                               keyType, loc);
                listManager.removeAt(keys, index, loc);
                for (size_t i = 0; i < types.size(); ++i) {
                    mlir::Value column = parts[kMapValuesLeafPos + i];
                    releaseElement(listManager.loadElement(column, index, node->line, loc, /*checked=*/false),
                                   types[i], loc);
                    listManager.removeAt(column, index, loc);
                }
            });
            lastValue = found;
            return;
        }

        // Anahtarların yeni bir listesi; string anahtarların başvuruları artar.
        case Builtin::Keys: {
            mlir::Value result = listManager.create(typeManager.convert(keyType), {}, loc);
            mlir::Value zeroIndex = b.create<mlir::arith::ConstantIndexOp>(loc, 0);
            mlir::Value one = b.create<mlir::arith::ConstantIndexOp>(loc, 1);
            b.create<mlir::scf::ForOp>(
                loc, zeroIndex, listManager.length(keys, loc), one, mlir::ValueRange(),
                [&](mlir::OpBuilder& nb, mlir::Location l, mlir::Value index, mlir::ValueRange) {
                    mlir::Value key = listManager.loadElement(keys, index, node->line, l, /*checked=*/false);
                    if (keyType.isString()) retainHandle(key, keyType, l);
                    listManager.push(result, key, l);
                    nb.create<mlir::scf::YieldOp>(l);
                });
            lastValue = result;
            return;
        }

        default:
            return;
    }
}
