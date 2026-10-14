// MLIRGenerator — liste elemanları ve struct listeleri
//
// list<Nokta> tek bir liste değildir: elemanın her yaprağı için ayrı bir
// liste ("sütun") tutulur (bkz. SemType.h, leavesOf). Buradaki her işlem,
// sütunlara tek tek uygulanan sıradan liste işlemidir:
//
//   push(l, p)     her sütuna p'nin o yaprağı eklenir
//   l[i]           her sütunun i. elemanı okunur  → struct değeri
//   l[i].x         yalnızca x sütunundan okunur
//   l[i].x = v     yalnızca x sütununa yazılır
//
// Sütunlar hep birlikte büyüyüp küçüldüğü için uzunlukları eşittir; indeks
// yalnızca ilk sütunda denetlenir.
//
// ELEMAN OLARAK MATRİS, DİZİ VE LİSTE
//
// Struct'ın alanı sayı ya da string değilse sütunun elemanı bir bellektir:
//
//   matris  elemanın KENDİ belleği (matris değerdir). Listeye yazılırken
//           sahiplenilir (yeni hesaplandıysa devralınır, bir değişkense
//           kopyalanır); üzerine yazılınca ve free(l) ile bırakılır. Okunan
//           değer ödünçtür; bir değişkene yazılırsa kopyalanır.
//   dizi    elemana ait, yığın belleğinde ayrı bir dizi (değişkenlerin dizisi
//           yığıtta yaşar, listeden kısa ömürlüdür). Yazılırken kopyalanır,
//           free(l) ile bırakılır.
//   liste   başvuru: sütunda yalnızca listenin hücresi durur. İki eleman aynı
//           listeyi paylaşabilir; free(l) onu bırakmaz (stringler gibi).
//
// Aynı kod sıradan list<T> için de çalışır: tek sütunu vardır.

#include "MLIRGenerator.h"

#include "mlir/Dialect/Arith/IR/Arith.h"
#include "mlir/Dialect/MemRef/IR/MemRef.h"
#include "mlir/Dialect/SCF/IR/SCF.h"

// ===========================================================================
// Elemanın saklanan hali
// ===========================================================================

mlir::Value MLIRGenerator::adoptElement(mlir::Value value, const SemType& type, int line,
                                        mlir::Location loc) {
    if (type.isMatrix()) return takeMatrixOwnership(value, line, loc);
    if (type.isString()) return ownString(value, loc);
    if (type.isArray()) {
        auto memRefType = mlir::cast<mlir::MemRefType>(typeManager.convert(type));
        mlir::Value copy = ctx.builder.create<mlir::memref::AllocOp>(loc, memRefType);
        copyArray(value, copy, type, /*releaseOld=*/false, loc);
        return copy;
    }
    return value;
}

mlir::Value MLIRGenerator::borrowElement(mlir::Value stored, const SemType& type,
                                         mlir::Location loc) {
    // Matris, ifade içinde tensor olarak görülür (salt okunur: bellek listenindir).
    if (type.isMatrix()) return matrixManager.toTensor(stored, /*writable=*/false, loc);
    // String: okunan değer bu deyimin başvurusu olur (bkz. MLIRGeneratorString.cpp).
    if (type.isString()) {
        retainString(stored, loc);
        stringResult(stored);
    }
    return stored;
}

void MLIRGenerator::releaseElement(mlir::Value stored, const SemType& type, mlir::Location loc) {
    if (type.isMatrix()) {
        matrixManager.freeBuffer(stored, loc);
    } else if (type.isString()) {
        releaseString(stored, loc);
    } else if (type.isArray()) {
        if (holdsStrings(type)) releaseStringArray(stored, type, loc);
        ctx.builder.create<mlir::memref::DeallocOp>(loc, stored);
    }
}

// ===========================================================================
// Okuma
// ===========================================================================

MLIRGenerator::Values MLIRGenerator::emitElement(ArrayAccessExpr* node, size_t begin, size_t count) {
    auto loc = locOf(node);
    Expression* list = node->target.get();
    const std::vector<SemLeaf> columns = leavesOf(list->type);

    Values cells = emitLeaves(list, begin, count);
    mlir::Value index = toIndex(emitExpr(node->indices[0].get()), node->indices[0]->type, loc);

    Values values;
    for (size_t i = 0; i < count; ++i) {
        mlir::Value stored = listManager.loadElement(cells[i], index, node->line, loc,
                                                     /*checked=*/i == 0);
        values.push_back(borrowElement(stored, columns[begin + i].type.elementType(), loc));
    }
    return values;
}

// [Nokta(1, 2), Nokta(3, 4)]  ya da  []
MLIRGenerator::Values MLIRGenerator::emitStructListLiteral(ListLiteral* node) {
    auto loc = locOf(node);
    const std::vector<SemLeaf> columns = leavesOf(node->type);

    std::vector<Values> stored(columns.size());
    for (auto& elem : node->elements) {
        Values values = emitLeaves(elem.get());
        for (size_t i = 0; i < columns.size(); ++i)
            stored[i].push_back(
                adoptElement(values[i], columns[i].type.elementType(), node->line, loc));
    }

    Values cells;
    for (size_t i = 0; i < columns.size(); ++i)
        cells.push_back(listManager.create(
            typeManager.convert(columns[i].type.elementType()), stored[i], loc));
    return cells;
}

// ===========================================================================
// push / pop / free
// ===========================================================================

void MLIRGenerator::emitListPush(FunctionCallExpr* node) {
    auto loc = locOf(node);
    Expression* list = node->arguments[0].get();
    const std::vector<SemLeaf> columns = leavesOf(list->type);

    Values cells = emitLeaves(list);
    Values values = emitLeaves(node->arguments[1].get());
    for (size_t i = 0; i < columns.size(); ++i)
        values[i] = adoptElement(values[i], columns[i].type.elementType(), node->line, loc);
    for (size_t i = 0; i < columns.size(); ++i)
        listManager.push(cells[i], values[i], loc);
}

// Çıkarılan eleman artık listeye ait değildir: matrisi bu deyimin geçicisi
// olur (sahiplenilmezse deyim sonunda bırakılır), dizisi yığıta kopyalanır.
MLIRGenerator::Values MLIRGenerator::emitListPop(FunctionCallExpr* node) {
    auto loc = locOf(node);
    Expression* list = node->arguments[0].get();
    const std::vector<SemLeaf> columns = leavesOf(list->type);

    Values cells = emitLeaves(list);
    Values values;
    for (size_t i = 0; i < columns.size(); ++i) {
        const SemType& type = columns[i].type.elementType();
        mlir::Value stored = listManager.pop(cells[i], node->line, loc);
        if (type.isMatrix()) {
            setMatrixResult({matrixManager.toTensor(stored, /*writable=*/false, loc), stored});
            values.push_back(lastValue);
        } else if (type.isArray()) {
            mlir::Value copy = createStorage(type, loc);
            ctx.builder.create<mlir::memref::CopyOp>(loc, stored, copy);
            ctx.builder.create<mlir::memref::DeallocOp>(loc, stored);
            // (Elemanların başvuruları kopyayla birlikte taşındı.)
            if (holdsStrings(type)) temporaries.push_back({copy, type});
            values.push_back(copy);
        } else {
            if (type.isString()) stringResult(stored); // listenin başvurusu deyime geçer
            values.push_back(stored);
        }
    }
    return values;
}

void MLIRGenerator::emitListFree(FunctionCallExpr* node) {
    auto loc = locOf(node);
    Expression* list = node->arguments[0].get();
    const std::vector<SemLeaf> columns = leavesOf(list->type);

    Values cells = emitLeaves(list);
    for (size_t i = 0; i < columns.size(); ++i) {
        const SemType& type = columns[i].type.elementType();
        // Elemanların kendi bellekleri (matris, dizi) ve tuttukları stringler
        // önce bırakılır.
        if (type.isMatrix() || type.isArray() || type.isString()) {
            mlir::Value zero = ctx.builder.create<mlir::arith::ConstantIndexOp>(loc, 0);
            mlir::Value one = ctx.builder.create<mlir::arith::ConstantIndexOp>(loc, 1);
            mlir::Value length = listManager.length(cells[i], loc);
            mlir::Value cell = cells[i];
            ctx.builder.create<mlir::scf::ForOp>(
                loc, zero, length, one, mlir::ValueRange(),
                [&](mlir::OpBuilder& b, mlir::Location l, mlir::Value index, mlir::ValueRange) {
                    mlir::Value stored = listManager.loadElement(cell, index, node->line, l,
                                                                 /*checked=*/false);
                    releaseElement(stored, type, l);
                    b.create<mlir::scf::YieldOp>(l);
                });
        }
        listManager.free(cells[i], loc);
    }
}

// ===========================================================================
// Yazma:  liste[i] = deger;   liste[i].alan = deger;   liste[i].a.b = deger;
// ===========================================================================

void MLIRGenerator::assignElement(AssignmentStmt* node) {
    auto loc = locOf(node);

    // Hedef, elemanın hangi yapraklarına düşüyor?
    size_t begin = 0;
    const size_t count = leavesOf(node->target->type).size();
    Expression* target = node->target.get();
    while (auto* member = dynamic_cast<MemberExpr*>(target)) {
        begin += leafOffset(member);
        target = member->base.get();
    }
    auto* access = static_cast<ArrayAccessExpr*>(target); // Sema doğruladı
    Expression* list = access->target.get();
    const std::vector<SemLeaf> columns = leavesOf(list->type);

    Values cells = emitLeaves(list, begin, count);
    mlir::Value index = toIndex(emitExpr(access->indices[0].get()), access->indices[0]->type, loc);
    Values values = emitLeaves(node->value.get());

    // Sağ taraf elemanın eski değerini kullanmış olabilir (l[i] = f(l[i])).
    // Önce bütün yeni değerler (matris bellekleri, stringler) sahiplenilir,
    // eskiler ancak ondan sonra bırakılır.
    for (size_t i = 0; i < count; ++i) {
        const SemType& type = columns[begin + i].type.elementType();
        if (type.isMatrix() || type.isString())
            values[i] = adoptElement(values[i], type, node->line, loc);
    }

    for (size_t i = 0; i < count; ++i) {
        const SemType& type = columns[begin + i].type.elementType();
        const bool checked = i == 0;
        if (type.isArray()) {
            // Elemanın dizisi yerinde kalır; içeriği değişir.
            mlir::Value stored = listManager.loadElement(cells[i], index, node->line, loc, checked);
            copyArray(values[i], stored, type, /*releaseOld=*/true, loc);
        } else if (type.isMatrix() || type.isString()) {
            mlir::Value old = listManager.loadElement(cells[i], index, node->line, loc, checked);
            listManager.storeElement(cells[i], index, values[i], node->line, loc, /*checked=*/false);
            releaseElement(old, type, loc);
        } else {
            listManager.storeElement(cells[i], index, values[i], node->line, loc, checked);
        }
    }
    flushTemporaries(loc);
}

// ===========================================================================
// for p in liste
//
// Döngü değişkeni "p.x", "p.y" adlı yapraklardır; her yinelemede listeden
// okunurlar. Matris ve dizi yaprakları listedeki elemanın belleğine doğrudan
// bağlanır (kopyalanmaz; Sema değiştirilmelerine izin vermez).
// ===========================================================================

void MLIRGenerator::emitStructListLoop(ForCollectionStmt* node) {
    auto loc = locOf(node);
    const std::vector<SemLeaf> columns = leavesOf(node->collection->type);
    const std::vector<SemLeaf> fields = leavesOf(node->iterType, node->iteratorName);

    Values cells = emitLeaves(node->collection.get());
    flushTemporaries(loc);

    // Uzunluk döngüye girerken bir kez okunur; eleman her yinelemede sınır
    // denetimiyle okunur (gövde listeyi kısaltabilir).
    mlir::Type indexType = ctx.builder.getIndexType();
    mlir::Value zero = arithManager.buildIntConstant(0, indexType, loc);
    mlir::Value size = listManager.length(cells[0], loc);
    mlir::Value one = arithManager.buildIntConstant(1, indexType, loc);

    cfManager.buildCountedLoop(loc, zero, size, one, /*isSigned=*/true,
        /*structured=*/!node->needsCFG, node->hasContinue,
        [&](mlir::Value index) {
            scopeManager.pushScope();
            llvm::SmallVector<Owned, 4> iterators;
            for (size_t i = 0; i < columns.size(); ++i) {
                const SemType& type = fields[i].type;
                mlir::Value stored = listManager.loadElement(cells[i], index, node->line, loc,
                                                             /*checked=*/i == 0);
                // Liste yaprağı bir yuvada tutulur (değişkenlerdeki gibi);
                // diğerleri doğrudan bağlanır.
                if (type.isList() || type.isString()) {
                    mlir::Value storage = createStorage(type, loc);
                    storeTo(stored, storage, type, loc);
                    // String yineleme boyunca kendi başvurusunu tutar (gövde
                    // elemanı listeden çıkarsa da değer geçerli kalır).
                    if (type.isString()) {
                        retainString(stored, loc);
                        iterators.push_back({storage, type});
                    }
                    stored = storage;
                }
                scopeManager.declareVariable(fields[i].path, stored);
            }
            emitLoopBody(node->body.get(), iterators);
            scopeManager.popScope();
        });
}
