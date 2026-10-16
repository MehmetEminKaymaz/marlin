// MLIRGenerator — scan: sorgu sonucunun satırlarını struct'lara doldurma
//
// scan(rows), bir satır kaynağının (std.db.Rows: handle + dört fonksiyon
// değeri) satırlarını, bağlamın istediği struct'a (S) ya da struct listesine
// (list<S>) yazar. Sürücüye özgü hiçbir şey bilmez; yalnızca Rows'un
// fonksiyonlarını çağırır:
//
//   n      = rows.count(h)
//   sütun  = rows.column(h, "alan")       her alan için bir kez; yoksa program durur
//   for satır in 0 .. n:                  (S için yalnızca satır 0)
//     null = rows.is_null(h, satır, sütun)
//     metin = rows.text(h, satır, sütun)
//     değer = null ? sıfır : metni alanın tipine çevir
//     l.alan'a ekle                       (list<S> alan başına bir listedir)
//
// Metinden sayıya çeviri, to_int / to_float ile aynı runtime fonksiyonlarıdır
// (çevrilemezse program satır bilgisiyle durur).

#include "MLIRGenerator.h"

#include "mlir/Dialect/Arith/IR/Arith.h"
#include "mlir/Dialect/SCF/IR/SCF.h"

mlir::Value MLIRGenerator::callFunctionValue(mlir::Value function, const SemType& type,
                                             llvm::ArrayRef<mlir::Value> args, mlir::Location loc) {
    mlir::Value callee = ctx.builder.create<mlir::UnrealizedConversionCastOp>(
        loc, mlir::TypeRange{typeManager.convertFunction(type)}, mlir::ValueRange{function}).getResult(0);
    auto call = ctx.builder.create<mlir::func::CallIndirectOp>(loc, callee, mlir::ValueRange(args));
    return call.getNumResults() ? call.getResult(0) : mlir::Value{};
}

MLIRGenerator::Values MLIRGenerator::emitScan(FunctionCallExpr* node) {
    auto loc = locOf(node);
    mlir::OpBuilder& b = ctx.builder;
    const SemType& sourceType = node->arguments[0]->type;
    const SemType& countType = sourceType.sub[1];
    const SemType& columnType = sourceType.sub[2];
    const SemType& textType = sourceType.sub[3];
    const SemType& nullType = sourceType.sub[4];
    const bool allRows = node->type.isList();
    const SemType row = allRows ? node->type.elementType() : node->type;
    mlir::Value line = arithManager.buildIntConstant(node->line, b.getI32Type(), loc);

    // Rows'un yaprakları: handle, count, column, text, is_null
    Values source = emitLeaves(node->arguments[0].get());
    mlir::Value handle = source[0];
    mlir::Value count = callFunctionValue(source[1], countType, {handle}, loc);

    // Her alanın sütunu, adıyla bir kez bulunur.
    Values columns;
    for (const std::string& fieldName : row.fieldNames) {
        StringLiteral name(fieldName);
        name.type = SemType::String();
        name.line = node->line;
        name.column = node->column;
        name.module = node->module;
        mlir::Value nameValue = emitExpr(&name);
        mlir::Value index = callFunctionValue(source[2], columnType, {handle, nameValue}, loc);
        mlir::Value zero = arithManager.buildIntConstant(0, b.getI64Type(), loc);
        mlir::Value missing = b.create<mlir::arith::CmpIOp>(loc, mlir::arith::CmpIPredicate::slt, index, zero);
        runtime.callIf(missing, "marlin_panic_no_column", {nameValue, line}, loc);
        columns.push_back(index);
    }

    // (satır, alan) değerini okur ve alanın tipine çevirir. String alanın
    // değeri yeni bir stringdir (sahibi çağırandır).
    auto readField = [&](mlir::Value rowIndex, size_t i) -> mlir::Value {
        const SemType& field = row.sub[i];
        mlir::Value isNull = callFunctionValue(source[4], nullType, {handle, rowIndex, columns[i]}, loc);
        mlir::Value text = callFunctionValue(source[3], textType, {handle, rowIndex, columns[i]}, loc);
        if (field.isString()) return text; // NULL: sürücü boş string verir

        mlir::Type type = typeManager.convert(field);
        auto ifOp = b.create<mlir::scf::IfOp>(loc, mlir::TypeRange{type}, isNull, /*withElseRegion=*/true);
        {
            mlir::OpBuilder::InsertionGuard guard(b);
            b.setInsertionPointToStart(&ifOp.getThenRegion().front());
            b.create<mlir::scf::YieldOp>(loc, emitZero(field, loc));

            b.setInsertionPointToStart(&ifOp.getElseRegion().front());
            mlir::Value value;
            if (field.isInt()) {
                value = runtime.callValue("marlin_str_to_i64", {text, line}, b.getI64Type(), loc);
                if (field.width < 64) value = b.create<mlir::arith::TruncIOp>(loc, type, value);
            } else if (field.isFloat()) {
                value = runtime.callValue("marlin_str_to_f64", {text, line}, b.getF64Type(), loc);
                if (field.width == 32) value = b.create<mlir::arith::TruncFOp>(loc, type, value);
            } else { // bool
                value = runtime.callValue("marlin_str_to_bool", {text, line}, b.getI32Type(), loc);
                value = b.create<mlir::arith::TruncIOp>(loc, type, value);
            }
            b.create<mlir::scf::YieldOp>(loc, value);
        }
        releaseString(text, loc);
        return ifOp.getResult(0);
    };

    // S: yalnızca ilk satır. String alanları bu deyimin geçicileridir;
    // değişkene yazılırken sahiplenilir.
    if (!allRows) {
        mlir::Value zero = arithManager.buildIntConstant(0, b.getI64Type(), loc);
        mlir::Value empty = b.create<mlir::arith::CmpIOp>(loc, mlir::arith::CmpIPredicate::eq, count, zero);
        runtime.callIf(empty, "marlin_panic_no_row", {line}, loc);
        Values values;
        for (size_t i = 0; i < row.sub.size(); ++i) {
            values.push_back(readField(zero, i));
            if (row.sub[i].isString()) stringResult(values.back());
        }
        return values;
    }

    // list<S>: alan başına bir liste (bkz. MLIRGeneratorList.cpp); elemanların
    // stringleri listeye aittir.
    Values cells;
    for (const SemType& field : row.sub)
        cells.push_back(listManager.create(typeManager.convert(field), {}, loc));
    mlir::Value begin = b.create<mlir::arith::ConstantIndexOp>(loc, 0);
    mlir::Value end = b.create<mlir::arith::IndexCastOp>(loc, b.getIndexType(), count);
    mlir::Value step = b.create<mlir::arith::ConstantIndexOp>(loc, 1);
    b.create<mlir::scf::ForOp>(
        loc, begin, end, step, mlir::ValueRange(),
        [&](mlir::OpBuilder& lb, mlir::Location l, mlir::Value index, mlir::ValueRange) {
            mlir::Value rowIndex = lb.create<mlir::arith::IndexCastOp>(l, lb.getI64Type(), index);
            for (size_t i = 0; i < row.sub.size(); ++i)
                listManager.push(cells[i], readField(rowIndex, i), l);
            lb.create<mlir::scf::YieldOp>(l);
        });
    return cells;
}
