// MLIRGenerator — struct'lar
//
// Struct tipli bir değerin MLIR'de tek bir karşılığı yoktur; yapraklarına
// (struct olmayan alanlarına; bkz. SemType.h, leavesOf) ayrılır:
//
//   * Değişken:  `Nokta p` → "p.x" ve "p.y" adlı iki ayrı yerel değişken.
//     Her biri kendi tipinin sıradan değişkenidir; bu yüzden `p.x` okumak,
//     `p.x = 3` yazmak, `p.dizi[i]`, `push(p.liste, 1)` için ayrıca kod
//     gerekmez: Sema bunları noktalı adlı sıradan isimler olarak bırakır.
//   * İfade:  struct değeri, yapraklarının değerlerinden oluşan bir listedir
//     (emitLeaves). Her yaprak, o tipte bir ifadenin değeriyle aynı
//     biçimdedir (matris tensor, dizi memref, liste hücre başvurusu ...).
//   * İşlem:  struct üzerindeki her işlem, yapraklara tek tek uygulanan aynı
//     işlemdir. `q = p` her alanı kendi kuralıyla atar: sayılar ve matrisler
//     kopyalanır (matris değerdir), listeler paylaşılır (liste başvurudur).
//
// Struct listesi (list<Nokta>) de aynı yolla, yaprak başına bir liste olarak
// taşınır; eleman işlemleri MLIRGeneratorList.cpp'dedir.
//
// Burada yalnızca struct'ın bütün olarak dolaştığı yerler vardır: oluşturma,
// tanımlama, atama ve alan seçme. Fonksiyona verme ve döndürme, çağrı ve
// return üretiminin içindedir (emitCall, emitReturn, emitFunctionBody).

#include "MLIRGenerator.h"

#include "mlir/Dialect/MemRef/IR/MemRef.h"

MLIRGenerator::Values MLIRGenerator::emitLeaves(Expression* expr) {
    return emitLeaves(expr, 0, leavesOf(expr->type).size());
}

// Yalnızca istenen aralık üretilir: `l[i].x` listenin tek bir sütunundan
// okur, elemanın bütün alanlarını değil.
MLIRGenerator::Values MLIRGenerator::emitLeaves(Expression* expr, size_t begin, size_t count) {
    // Bileşik olmayan ifadenin tek yaprağı kendi değeridir.
    if (!expr->type.isComposite()) return {emitExpr(expr)};

    // Değişken (ya da bir değişkenin struct tipli alanı): yaprakları okunur.
    if (auto* ident = dynamic_cast<Identifier*>(expr)) {
        const std::vector<SemLeaf> leaves = leavesOf(ident->type, ident->name);
        Values values;
        for (size_t i = begin; i < begin + count; ++i) {
            Identifier field(leaves[i].path);
            field.ref = ident->ref == Identifier::Ref::Global ? Identifier::Ref::Global
                                                              : Identifier::Ref::Local;
            field.type = leaves[i].type;
            field.line = ident->line;
            field.column = ident->column;
            field.module = ident->module;
            values.push_back(emitExpr(&field));
        }
        return values;
    }

    // Alan: tabandaki struct'ın yaprakları sıralıdır; alanın yaprakları,
    // kendinden önceki alanların yapraklarından sonra gelir.
    if (auto* member = dynamic_cast<MemberExpr*>(expr))
        return emitLeaves(member->base.get(), leafOffset(member) + begin, count);

    // Liste ya da map elemanı: l[i], m[k]
    if (auto* access = dynamic_cast<ArrayAccessExpr*>(expr))
        return access->target->type.isMap() ? emitMapElement(access, begin, count)
                                            : emitElement(access, begin, count);

    // Kalan ifadeler bütün olarak üretilir; istenen aralık sonuçtan seçilir.
    Values all = emitWholeLeaves(expr);
    return Values(all.begin() + begin, all.begin() + begin + count);
}

MLIRGenerator::Values MLIRGenerator::emitWholeLeaves(Expression* expr) {
    auto loc = locOf(expr);

    if (auto* list = dynamic_cast<ListLiteral*>(expr))
        return list->type.isMap() ? emitMapLiteral(list->type, loc) : emitStructListLiteral(list);
    auto* call = static_cast<FunctionCallExpr*>(expr);
    if (call->builtin == Builtin::Pop) return emitListPop(call);
    if (call->builtin == Builtin::Scan) return emitScan(call);

    // Nokta(1.0, 2.0): argümanlar alanların değerleridir.
    if (call->isConstructor) {
        Values values;
        for (auto& arg : call->arguments) {
            if (arg->type.isComposite()) {
                Values nested = emitLeaves(arg.get());
                values.append(nested.begin(), nested.end());
                continue;
            }
            mlir::Value value = emitExpr(arg.get());
            // Dizi ifadesinin değeri dizinin kendisidir (başvuru). Alan ise
            // ayrı bir kopyadır; değer hemen kopyalanır ki sonradan değişen
            // kaynak (ör. p = Kayit(p.b, p.a)) sonucu etkilemesin.
            if (arg->type.isArray() && !dynamic_cast<ListLiteral*>(arg.get())) {
                mlir::Value copy = createStorage(arg->type, loc);
                copyArray(value, copy, arg->type, /*releaseOld=*/false, loc);
                if (holdsStrings(arg->type)) temporaries.push_back({copy, arg->type});
                value = copy;
            }
            values.push_back(value);
        }
        return values;
    }

    // Struct döndüren çağrı. Dönen matrisler yeni belleklerdir ve artık bu
    // deyime aittir (sahiplenilmezlerse deyim sonunda bırakılırlar).
    Values values = emitCall(call);
    const std::vector<SemLeaf> leaves = leavesOf(call->type);
    for (size_t i = 0; i < leaves.size(); ++i) {
        if (!leaves[i].type.isMatrix()) continue;
        setMatrixResult({matrixManager.toTensor(values[i], /*writable=*/false, loc), values[i]});
        values[i] = lastValue;
    }
    return values;
}

// Alanın, tabandaki struct'ın yaprakları içindeki başlangıç sırası.
size_t MLIRGenerator::leafOffset(const MemberExpr* node) {
    const SemType& baseType = node->base->type;
    size_t offset = 0;
    for (int i = 0; i < node->fieldIndex; ++i)
        offset += leavesOf(baseType.sub[i]).size();
    return offset;
}

MLIRGenerator::Values MLIRGenerator::emitMember(MemberExpr* node) {
    return emitLeaves(node->base.get(), leafOffset(node), leavesOf(node->type).size());
}
// Nokta p = ...;   Nokta p;
void MLIRGenerator::declareStruct(VariableDecl* node) {
    auto loc = locOf(node);
    const std::vector<SemLeaf> leaves = leavesOf(node->varType, node->name);

    if (!node->initializer) {
        // Listeler boş başlar; map'in tablosu (yaprağı ".#index") yeni bir
        // tablo olur: `map<K, V> m;` boş bir map'tir.
        for (const SemLeaf& leaf : leaves) {
            if (llvm::StringRef(leaf.path).ends_with(kMapIndexLeaf))
                declareLocal(leaf.path, leaf.type,
                             runtime.callValue("marlin_map_new", {}, ctx.builder.getI64Type(), loc),
                             node->line, loc);
            else
                declareDefault(leaf.path, leaf.type, loc);
        }
        return;
    }
    Values values = emitLeaves(node->initializer.get());
    for (size_t i = 0; i < leaves.size(); ++i)
        declareLocal(leaves[i].path, leaves[i].type, values[i], node->line, loc);
    flushTemporaries(loc);
}

// p = ...;   c.merkez = ...;
void MLIRGenerator::assignStruct(AssignmentStmt* node, Identifier* target) {
    auto loc = locOf(node);
    const std::vector<SemLeaf> leaves = leavesOf(target->type, target->name);
    Values values = emitLeaves(node->value.get());

    // Sağ taraf hedefin eski alanlarını kullanmış olabilir (p = Nokta(p.y, p.x)).
    // Önce bütün yeni matris bellekleri hazırlanır, eskiler ancak ondan sonra
    // bırakılır.
    for (size_t i = 0; i < leaves.size(); ++i) {
        if (leaves[i].type.isMatrix())
            values[i] = takeMatrixOwnership(values[i], node->line, loc);
        else if (leaves[i].type.isString())
            values[i] = ownString(values[i], loc);
    }

    const bool isGlobal = target->ref == Identifier::Ref::Global;
    for (size_t i = 0; i < leaves.size(); ++i) {
        mlir::Value storage = isGlobal ? emitGlobalRef(leaves[i].path, loc)
                                       : scopeManager.lookupVariable(leaves[i].path);
        if (leaves[i].type.isMatrix()) {
            mlir::Value oldBuffer = ctx.builder.create<mlir::memref::LoadOp>(loc, storage);
            ctx.builder.create<mlir::memref::StoreOp>(loc, values[i], storage);
            matrixManager.freeBuffer(oldBuffer, loc);
        } else if (leaves[i].type.isString()) {
            mlir::Value oldValue = ctx.builder.create<mlir::memref::LoadOp>(loc, storage);
            ctx.builder.create<mlir::memref::StoreOp>(loc, values[i], storage);
            releaseString(oldValue, loc);
        } else if (leaves[i].type.isArray()) {
            copyArray(values[i], storage, leaves[i].type, /*releaseOld=*/true, loc);
        } else {
            storeTo(values[i], storage, leaves[i].type, loc);
        }
    }
    flushTemporaries(loc);
}
