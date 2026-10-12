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
//     (emitStructExpr). Her yaprak, o tipte bir ifadenin değeriyle aynı
//     biçimdedir (matris tensor, dizi memref, liste hücre başvurusu ...).
//   * İşlem:  struct üzerindeki her işlem, yapraklara tek tek uygulanan aynı
//     işlemdir. `q = p` her alanı kendi kuralıyla atar: sayılar ve matrisler
//     kopyalanır (matris değerdir), listeler paylaşılır (liste başvurudur).
//
// Burada yalnızca struct'ın bütün olarak dolaştığı yerler vardır: oluşturma,
// tanımlama, atama ve alan seçme. Fonksiyona verme ve döndürme, çağrı ve
// return üretiminin içindedir (emitCall, emitReturn, emitFunctionBody).

#include "MLIRGenerator.h"

#include "mlir/Dialect/MemRef/IR/MemRef.h"

MLIRGenerator::Values MLIRGenerator::emitStructExpr(Expression* expr) {
    auto loc = locOf(expr);

    // Değişken (ya da bir değişkenin struct tipli alanı): yaprakları okunur.
    if (auto* ident = dynamic_cast<Identifier*>(expr)) {
        Values values;
        for (const SemLeaf& leaf : leavesOf(ident->type, ident->name)) {
            Identifier field(leaf.path);
            field.ref = Identifier::Ref::Local;
            field.type = leaf.type;
            field.line = ident->line;
            field.column = ident->column;
            field.module = ident->module;
            values.push_back(emitExpr(&field));
        }
        return values;
    }

    if (auto* member = dynamic_cast<MemberExpr*>(expr))
        return emitMember(member);

    auto* call = static_cast<FunctionCallExpr*>(expr);

    // Nokta(1.0, 2.0): argümanlar alanların değerleridir.
    if (call->isConstructor) {
        Values values;
        for (auto& arg : call->arguments) {
            if (arg->type.isStruct()) {
                Values nested = emitStructExpr(arg.get());
                values.append(nested.begin(), nested.end());
                continue;
            }
            mlir::Value value = emitExpr(arg.get());
            // Dizi ifadesinin değeri dizinin kendisidir (başvuru). Alan ise
            // ayrı bir kopyadır; değer hemen kopyalanır ki sonradan değişen
            // kaynak (ör. p = Kayit(p.b, p.a)) sonucu etkilemesin.
            if (arg->type.isArray() && !dynamic_cast<ListLiteral*>(arg.get())) {
                mlir::Value copy = createStorage(arg->type, loc);
                storeTo(value, copy, arg->type, loc);
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

// Tabandaki struct'ın yaprakları sıralıdır; alanın yaprakları, kendinden
// önceki alanların yapraklarından sonra gelir.
MLIRGenerator::Values MLIRGenerator::emitMember(MemberExpr* node) {
    Values base = emitStructExpr(node->base.get());
    const SemType& baseType = node->base->type;

    size_t begin = 0;
    for (int i = 0; i < node->fieldIndex; ++i)
        begin += leavesOf(baseType.sub[i]).size();
    const size_t count = leavesOf(node->type).size();
    return Values(base.begin() + begin, base.begin() + begin + count);
}

// Nokta p = ...;   Nokta p;
void MLIRGenerator::declareStruct(VariableDecl* node) {
    auto loc = locOf(node);
    const std::vector<SemLeaf> leaves = leavesOf(node->varType, node->name);

    if (!node->initializer) {
        for (const SemLeaf& leaf : leaves) declareDefault(leaf.path, leaf.type, loc);
        return;
    }
    Values values = emitStructExpr(node->initializer.get());
    for (size_t i = 0; i < leaves.size(); ++i)
        declareLocal(leaves[i].path, leaves[i].type, values[i], node->line, loc);
    flushTemporaries(loc);
}

// p = ...;   c.merkez = ...;
void MLIRGenerator::assignStruct(AssignmentStmt* node, Identifier* target) {
    auto loc = locOf(node);
    const std::vector<SemLeaf> leaves = leavesOf(target->type, target->name);
    Values values = emitStructExpr(node->value.get());

    // Sağ taraf hedefin eski alanlarını kullanmış olabilir (p = Nokta(p.y, p.x)).
    // Önce bütün yeni matris bellekleri hazırlanır, eskiler ancak ondan sonra
    // bırakılır.
    for (size_t i = 0; i < leaves.size(); ++i)
        if (leaves[i].type.isMatrix())
            values[i] = takeMatrixOwnership(values[i], node->line, loc);

    for (size_t i = 0; i < leaves.size(); ++i) {
        mlir::Value storage = scopeManager.lookupVariable(leaves[i].path);
        if (leaves[i].type.isMatrix()) {
            mlir::Value oldBuffer = ctx.builder.create<mlir::memref::LoadOp>(loc, storage);
            ctx.builder.create<mlir::memref::StoreOp>(loc, values[i], storage);
            matrixManager.freeBuffer(oldBuffer, loc);
        } else {
            storeTo(values[i], storage, leaves[i].type, loc);
        }
    }
    flushTemporaries(loc);
}
