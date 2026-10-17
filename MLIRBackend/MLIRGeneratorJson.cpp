// MLIRGenerator — from_json / to_json (bkz. std/json.marl)
//
// İkisi de dilde jenerik fonksiyon olmadığı için yerleşiktir: kod, derlerken
// bilinen tipe göre üretilir. Tip iç içe olabilir (struct içinde liste, map
// değeri struct ...); üretim tipin yapısı boyunca özyinelemelidir. Çalışma
// zamanında yalnızca runtime/std_json.c'deki küçük fonksiyonlar çağrılır.
//
// from_json(v), T:
//
//   sayı / bool / string   marlin_json_decode_<tip>(belge, düğüm)
//   struct                 her alan: düğüm = get(nesne, "alan"); alanı çöz
//   list<T>                her eleman: düğüm = at(dizi, i); çöz; sütunlara ekle
//   map<string, V>         her üye: anahtar = key_at(nesne, i); değeri çöz; ekle
//
// Olmayan düğüm (-1) her fonksiyonda sıfır değer verir; böylece eksik alan ve
// olmayan dizi için ayrıca dallanmak gerekmez. Yanlış tipler hatayı belgeye
// kaydeder (marlin_json_decode_expect).
//
// SAHİPLİK: çözülen her string yenidir. Liste ve map içindekiler doğrudan
// listeye eklenir (sahipliği listeye geçer); en dıştaki değerin stringleri bu
// deyimin geçicileri olur (değişkene yazılınca sahiplenilir).
//
// to_json(x): yazıcıya (marlin_json_writer_*) parça parça yazılır; struct'ın
// "{"ad":" gibi sabit parçaları derlerken hazırlanır.

#include "MLIRGenerator.h"

#include "mlir/Dialect/Arith/IR/Arith.h"
#include "mlir/Dialect/MemRef/IR/MemRef.h"
#include "mlir/Dialect/SCF/IR/SCF.h"

namespace {

// JsonKind (runtime/json_parser.h)
constexpr int32_t kJsonArray = 5;
constexpr int32_t kJsonObject = 6;

bool isJsonValue(const SemType& type) { return type.isStruct() && type.name == "std$json$Value"; }

// "ad" → "\"ad\"" (alan adları tanımlayıcıdır; kaçış gerekmez)
std::string quoted(const std::string& name) { return "\"" + name + "\""; }

} // namespace

mlir::Value MLIRGenerator::jsonConstant(const std::string& text, const ASTNode* at) {
    StringLiteral literal(text);
    literal.type = SemType::String();
    literal.line = at->line;
    literal.column = at->column;
    literal.module = at->module;
    return emitExpr(&literal);
}

// 0 .. count arası her i için gövde (scf.for); i hem index hem i64 olarak verilir.
void MLIRGenerator::forEachJsonIndex(mlir::Value count, mlir::Location loc,
                                     llvm::function_ref<void(mlir::Value index, mlir::Value i64)> body) {
    mlir::OpBuilder& b = ctx.builder;
    mlir::Value begin = b.create<mlir::arith::ConstantIndexOp>(loc, 0);
    mlir::Value end = count.getType().isIndex() ? count
                                                : b.create<mlir::arith::IndexCastOp>(loc, b.getIndexType(), count);
    mlir::Value step = b.create<mlir::arith::ConstantIndexOp>(loc, 1);
    b.create<mlir::scf::ForOp>(
        loc, begin, end, step, mlir::ValueRange(),
        [&](mlir::OpBuilder& lb, mlir::Location l, mlir::Value index, mlir::ValueRange) {
            body(index, lb.create<mlir::arith::IndexCastOp>(l, lb.getI64Type(), index));
            lb.create<mlir::scf::YieldOp>(l);
        });
}

// ===========================================================================
// from_json
// ===========================================================================

MLIRGenerator::Values MLIRGenerator::emitFromJson(FunctionCallExpr* node) {
    auto loc = locOf(node);
    Values source = emitLeaves(node->arguments[0].get()); // json.Value: belge, düğüm
    runtime.call("marlin_json_decode_begin", {source[0]}, loc);
    Values values = decodeJson(node->type, source[0], source[1], node, loc);

    const std::vector<SemLeaf> leaves = leavesOf(node->type);
    for (size_t i = 0; i < leaves.size(); ++i)
        if (leaves[i].type.isString()) stringResult(values[i]);
    return values;
}

MLIRGenerator::Values MLIRGenerator::decodeJson(const SemType& type, mlir::Value doc, mlir::Value json,
                                                const ASTNode* at, mlir::Location loc) {
    mlir::OpBuilder& b = ctx.builder;
    mlir::Type i32 = b.getI32Type();
    mlir::Type i64 = b.getI64Type();
    auto constant = [&](int64_t v, mlir::Type t) { return arithManager.buildIntConstant(v, t, loc); };

    if (type.isInt()) {
        mlir::Value v = runtime.callValue(
            "marlin_json_decode_int", {doc, json, constant(type.width, i32), constant(type.isSigned, i32)}, i64, loc);
        if (type.width < 64) v = b.create<mlir::arith::TruncIOp>(loc, typeManager.convert(type), v);
        return {v};
    }
    if (type.isFloat()) {
        mlir::Value v = runtime.callValue("marlin_json_decode_float", {doc, json}, b.getF64Type(), loc);
        if (type.width == 32) v = b.create<mlir::arith::TruncFOp>(loc, b.getF32Type(), v);
        return {v};
    }
    if (type.isBool()) {
        mlir::Value v = runtime.callValue("marlin_json_decode_bool", {doc, json}, i32, loc);
        return {b.create<mlir::arith::TruncIOp>(loc, b.getI1Type(), v)};
    }
    if (type.isString())
        return {runtime.callValue("marlin_json_decode_string", {doc, json}, i64, loc)};

    // Struct: alanlar nesnenin anahtarlarına adlarıyla eşlenir.
    if (type.isStruct()) {
        runtime.callValue("marlin_json_decode_expect", {doc, json, constant(kJsonObject, i32)}, i32, loc);
        Values values;
        for (size_t i = 0; i < type.sub.size(); ++i) {
            mlir::Value child = runtime.callValue("marlin_json_get", {doc, json, jsonConstant(type.fieldNames[i], at)},
                                                  i64, loc);
            Values field = decodeJson(type.sub[i], doc, child, at, loc);
            values.append(field.begin(), field.end());
        }
        return values;
    }

    // list<T>: elemanın her yaprağı için bir sütun.
    if (type.isList()) {
        const SemType& elem = type.elementType();
        Values cells;
        for (const SemLeaf& leaf : leavesOf(type))
            cells.push_back(listManager.create(typeManager.convert(leaf.type.elementType()), {}, loc));
        runtime.callValue("marlin_json_decode_expect", {doc, json, constant(kJsonArray, i32)}, i32, loc);
        mlir::Value count = runtime.callValue("marlin_json_length", {doc, json}, i64, loc);
        forEachJsonIndex(count, loc, [&](mlir::Value, mlir::Value i) {
            mlir::Value child = runtime.callValue("marlin_json_at", {doc, json, i}, i64, loc);
            Values element = decodeJson(elem, doc, child, at, loc);
            for (size_t c = 0; c < cells.size(); ++c) listManager.push(cells[c], element[c], loc);
        });
        return cells;
    }

    // map<string, V>: nesnenin üyeleri, yazılış sırasıyla. Aynı anahtar birden
    // çok kez geçiyorsa yalnızca sonuncusu eklenir (get de sonuncuyu verir).
    if (type.isMap()) {
        Values parts = emitMapLiteral(type, loc);
        runtime.callValue("marlin_json_decode_expect", {doc, json, constant(kJsonObject, i32)}, i32, loc);
        mlir::Value count = runtime.callValue("marlin_json_length", {doc, json}, i64, loc);
        forEachJsonIndex(count, loc, [&](mlir::Value, mlir::Value i) {
            mlir::Value child = runtime.callValue("marlin_json_at", {doc, json, i}, i64, loc);
            mlir::Value key = runtime.callValue("marlin_json_key_at", {doc, json, i}, i64, loc);
            mlir::Value last = runtime.callValue("marlin_json_get", {doc, json, key}, i64, loc);
            mlir::Value isLast = b.create<mlir::arith::CmpIOp>(loc, mlir::arith::CmpIPredicate::eq, last, child);
            auto branch = b.create<mlir::scf::IfOp>(loc, mlir::TypeRange(), isLast,
                                                    /*addThenBlock=*/true, /*addElseBlock=*/true);
            {
                mlir::OpBuilder::InsertionGuard guard(b);
                b.setInsertionPointToEnd(&branch.getThenRegion().front());
                mlir::Value row = b.create<mlir::arith::IndexCastOp>(
                    loc, i64, listManager.length(parts[kMapKeysLeafPos], loc));
                listManager.push(parts[kMapKeysLeafPos], key, loc); // (anahtarın sahipliği listeye geçer)
                runtime.call("marlin_map_insert", {parts[kMapIndexLeafPos], key, constant(1, i32), row}, loc);
                Values value = decodeJson(type.valueType(), doc, child, at, loc);
                for (size_t c = 0; c < value.size(); ++c)
                    listManager.push(parts[kMapValuesLeafPos + c], value[c], loc);
                b.create<mlir::scf::YieldOp>(loc);
            }
            {
                mlir::OpBuilder::InsertionGuard guard(b);
                b.setInsertionPointToEnd(&branch.getElseRegion().front());
                releaseString(key, loc);
                b.create<mlir::scf::YieldOp>(loc);
            }
        });
        return parts;
    }
    llvm_unreachable("from_json: Sema desteklenmeyen tipi gecirdi");
}

// ===========================================================================
// to_json
// ===========================================================================

mlir::Value MLIRGenerator::emitToJson(FunctionCallExpr* node) {
    auto loc = locOf(node);
    Expression* arg = node->arguments[0].get();
    Values leaves = emitLeaves(arg);
    mlir::Value writer = runtime.callValue("marlin_json_writer_new", {}, ctx.builder.getI64Type(), loc);
    encodeJson(arg->type, leaves, writer, node, loc);
    return stringResult(runtime.callValue("marlin_json_writer_finish", {writer}, ctx.builder.getI64Type(), loc));
}

void MLIRGenerator::encodeJson(const SemType& type, llvm::ArrayRef<mlir::Value> leaves, mlir::Value writer,
                               const ASTNode* at, mlir::Location loc) {
    mlir::OpBuilder& b = ctx.builder;
    mlir::Type i32 = b.getI32Type();
    mlir::Type i64 = b.getI64Type();
    auto raw = [&](const std::string& text) {
        runtime.call("marlin_json_write_raw", {writer, jsonConstant(text, at)}, loc);
    };
    // Döngüde elemanlar arasına virgül: i > 0 ise.
    auto separator = [&](mlir::Value i) {
        mlir::Value zero = arithManager.buildIntConstant(0, i64, loc);
        mlir::Value notFirst = b.create<mlir::arith::CmpIOp>(loc, mlir::arith::CmpIPredicate::sgt, i, zero);
        runtime.ifThen(notFirst, loc, [&] { raw(","); });
    };
    mlir::Value value = leaves.empty() ? mlir::Value{} : leaves.front();

    if (type.isInt()) {
        const bool isSigned = type.isSigned;
        if (type.width < 64)
            value = isSigned ? b.create<mlir::arith::ExtSIOp>(loc, i64, value).getResult()
                             : b.create<mlir::arith::ExtUIOp>(loc, i64, value).getResult();
        runtime.call(isSigned ? "marlin_json_write_int" : "marlin_json_write_uint", {writer, value}, loc);
        return;
    }
    if (type.isFloat()) {
        runtime.call(type.width == 32 ? "marlin_json_write_f32" : "marlin_json_write_f64", {writer, value}, loc);
        return;
    }
    if (type.isBool() || type.isChar()) {
        mlir::Value wide = b.create<mlir::arith::ExtUIOp>(loc, i32, value);
        runtime.call(type.isBool() ? "marlin_json_write_bool" : "marlin_json_write_char", {writer, wide}, loc);
        return;
    }
    if (type.isString()) {
        runtime.call("marlin_json_write_string", {writer, value}, loc);
        return;
    }

    // json.Value: belgedeki değer olduğu gibi
    if (isJsonValue(type)) {
        runtime.call("marlin_json_write_value", {writer, leaves[0], leaves[1]}, loc);
        return;
    }

    // Struct: {"a":...,"b":...}  — alan adları ve ayraçlar sabit parçalardır.
    if (type.isStruct()) {
        size_t offset = 0;
        for (size_t i = 0; i < type.sub.size(); ++i) {
            raw((i == 0 ? "{" : ",") + quoted(type.fieldNames[i]) + ":");
            const size_t count = leavesOf(type.sub[i]).size();
            encodeJson(type.sub[i], leaves.slice(offset, count), writer, at, loc);
            offset += count;
        }
        raw("}");
        return;
    }

    // Sabit dizi: [e0,e1,...]
    if (type.isArray()) {
        raw("[");
        mlir::Value count = b.create<mlir::arith::ConstantIndexOp>(loc, type.arraySize);
        forEachJsonIndex(count, loc, [&](mlir::Value index, mlir::Value i) {
            separator(i);
            mlir::Value elem = b.create<mlir::memref::LoadOp>(loc, value, mlir::ValueRange(index));
            encodeJson(type.elementType(), {elem}, writer, at, loc);
        });
        raw("]");
        return;
    }

    // list<T>: her yinelemede elemanın yaprakları sütunlardan okunur.
    if (type.isList()) {
        raw("[");
        forEachJsonIndex(listManager.length(leaves[0], loc), loc, [&](mlir::Value index, mlir::Value i) {
            separator(i);
            Values element;
            for (mlir::Value cell : leaves)
                element.push_back(listManager.loadElement(cell, index, at->line, loc, /*checked=*/false));
            encodeJson(type.elementType(), element, writer, at, loc);
        });
        raw("]");
        return;
    }

    // map<K, V>: {"anahtar":değer,...}, eklenme sırasıyla. JSON'da anahtar
    // stringdir; diğer anahtarlar tırnak içinde yazılır ("1", "true").
    if (type.isMap()) {
        const SemType& keyType = type.keyType();
        mlir::Value keys = leaves[kMapKeysLeafPos];
        raw("{");
        forEachJsonIndex(listManager.length(keys, loc), loc, [&](mlir::Value index, mlir::Value i) {
            separator(i);
            mlir::Value key = listManager.loadElement(keys, index, at->line, loc, /*checked=*/false);
            if (keyType.isString()) {
                runtime.call("marlin_json_write_string", {writer, key}, loc);
            } else {
                const int32_t kind = keyType.isBool() ? 3 : keyType.isChar() ? 2 : keyType.isSigned ? 0 : 1;
                runtime.call("marlin_json_write_key",
                             {writer, toMapKey(key, keyType, loc), arithManager.buildIntConstant(kind, i32, loc)},
                             loc);
            }
            raw(":");
            Values element;
            for (size_t c = kMapValuesLeafPos; c < leaves.size(); ++c)
                element.push_back(listManager.loadElement(leaves[c], index, at->line, loc, /*checked=*/false));
            encodeJson(type.valueType(), element, writer, at, loc);
        });
        raw("}");
        return;
    }
    llvm_unreachable("to_json: Sema desteklenmeyen tipi gecirdi");
}
