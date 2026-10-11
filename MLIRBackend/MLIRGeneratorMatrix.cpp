// MLIRGenerator — matris ifadeleri ve matris belleğinin sahipliği
//
// SAHİPLİK KURALLARI
//
// Matrisler değer olarak davranır: iki değişken asla aynı belleği paylaşmaz.
// Bunu sağlamak ve belleği sızdırmamak için her matris belleğinin her an tek
// bir sahibi vardır:
//
//   * Yeni hesaplanan her matris (işlem sonucu, zeros(...), fonksiyonun
//     döndürdüğü değer, literal) bir GEÇİCİdir ve içinde bulunulan deyime
//     aittir. Deyim bitince bırakılır (flushTemporaries).
//
//   * Bir geçici bir değişkene yazılırsa ya da return edilirse sahiplik oraya
//     geçer (takeMatrixOwnership); kopyalanmaz.
//
//   * Bir değişkenin değeri başka bir değişkene yazılırsa kopyası çıkarılır:
//     `b = a` sonrasında a ve b bağımsızdır.
//
//   * Değişkene yeni değer atanınca eski belleği bırakılır.
//
//   * Blok bitince, o blokta tanımlanan matris değişkenleri bırakılır
//     (freeMatrixScopes). return / break / continue ile çıkışlarda da aynısı.
//
//   * Parametreler ödünç alınmıştır: çağıranın belleğidir, fonksiyon onu ne
//     değiştirir (Sema engeller) ne de bırakır.

#include "MLIRGenerator.h"

#include "mlir/Dialect/Arith/IR/Arith.h"
#include "mlir/Dialect/MemRef/IR/MemRef.h"
#include "mlir/Dialect/SCF/IR/SCF.h"
#include <algorithm>

// ===========================================================================
// Sahiplik
// ===========================================================================

void MLIRGenerator::setMatrixResult(MLIRMatrixManager::Result result) {
    temporaries.push_back(result.buffer);
    ownedBuffers[result.tensor] = result.buffer;
    lastValue = result.tensor;
}

mlir::Value MLIRGenerator::takeMatrixOwnership(mlir::Value tensor, int line, mlir::Location loc) {
    // Yeni hesaplanmış bir geçici: belleği olduğu gibi devral.
    auto owned = ownedBuffers.find(tensor);
    if (owned != ownedBuffers.end()) {
        auto position = std::find(temporaries.begin(), temporaries.end(), owned->second);
        if (position != temporaries.end()) temporaries.erase(position);
        ownedBuffers.erase(owned);
        return matrixManager.toBuffer(tensor, loc);
    }

    // Başkasına ait (bir değişkenin ya da parametrenin değeri): kopyala.
    mlir::Type elementType = mlir::cast<mlir::ShapedType>(tensor.getType()).getElementType();
    auto copy = matrixManager.elementwise(
        {tensor}, elementType, line,
        [](mlir::ValueRange elements) -> mlir::Value { return elements[0]; }, loc);
    return matrixManager.toBuffer(copy.tensor, loc);
}

void MLIRGenerator::flushTemporaries(mlir::Location loc, size_t from) {
    for (size_t i = from; i < temporaries.size(); ++i) {
        mlir::Value buffer = temporaries[i];
        matrixManager.freeBuffer(buffer, loc);
        for (auto it = ownedBuffers.begin(); it != ownedBuffers.end(); ++it) {
            if (it->second != buffer) continue;
            ownedBuffers.erase(it);
            break;
        }
    }
    temporaries.resize(std::min(from, temporaries.size()));
}

void MLIRGenerator::freeMatrixScopes(size_t fromDepth, mlir::Location loc, mlir::Value keep) {
    for (size_t depth = fromDepth; depth < matrixScopes.size(); ++depth) {
        for (mlir::Value slot : matrixScopes[depth]) {
            if (slot == keep) continue;
            mlir::Value buffer = ctx.builder.create<mlir::memref::LoadOp>(loc, slot);
            matrixManager.freeBuffer(buffer, loc);
        }
    }
}

mlir::Value MLIRGenerator::ownedMatrixSlot(Expression* expr) {
    auto* ident = dynamic_cast<Identifier*>(expr);
    if (!ident || ident->ref != Identifier::Ref::Local || !ident->type.isMatrix())
        return {};
    mlir::Value storage = scopeManager.lookupVariable(ident->name);
    for (auto& scope : matrixScopes)
        if (std::find(scope.begin(), scope.end(), storage) != scope.end())
            return storage;
    return {}; // parametre: sahibi biz değiliz
}

mlir::Value MLIRGenerator::emitMatrixBuffer(Expression* expr) {
    // Değişken: belleği doğrudan kullan (tensor'a çevirip geri almaya gerek yok).
    auto* ident = dynamic_cast<Identifier*>(expr);
    if (ident && ident->ref == Identifier::Ref::Local)
        return loadFrom(scopeManager.lookupVariable(ident->name), ident->type, locOf(expr));
    return matrixManager.toBuffer(emitExpr(expr), locOf(expr));
}

// ===========================================================================
// Eleman eleman ifadeler — füzyon
//
// `a + b * 2 - c` gibi bir ifade üç ayrı döngü ve iki geçici matris yerine
// TEK bir işlem olarak üretilir. Bunun için ifade ağacı önce bir plana
// çevrilir:
//
//   * girdiler: eleman eleman olmayan her matris alt ifadesi (değişken,
//     matmul sonucu, fonksiyon çağrısı ...). Bunlar önce hesaplanır.
//   * skalerler: sayı alt ifadeleri; onlar da önce, bir kez hesaplanır.
//   * düğümler: girdilerin elemanları üzerinde yapılacak hesap.
//
// Sonra plan, gövdesi bu hesabın tamamı olan tek bir linalg.generic olarak
// üretilir.
// ===========================================================================

struct MLIRGenerator::MatrixExprNode {
    enum Kind { Input, Scalar, Binary, Negate, Cast };

    Kind kind = Input;
    unsigned inputIndex = 0;            // Input: plan.inputs içindeki sırası
    mlir::Value scalar;                 // Scalar: önceden hesaplanmış değer
    BinaryExpr::Op op = BinaryExpr::Add;
    SemType type;                       // düğümün ürettiği değerin (eleman) tipi
    SemType fromType;                   // Cast: kaynağın eleman tipi
    std::unique_ptr<MatrixExprNode> lhs, rhs;
};

struct MLIRGenerator::MatrixExprPlan {
    llvm::SmallVector<mlir::Value, 4> inputs; // girdi matrisleri (tensor)
};

std::unique_ptr<MLIRGenerator::MatrixExprNode>
MLIRGenerator::planMatrixExpr(Expression* expr, MatrixExprPlan& plan) {
    auto node = std::make_unique<MatrixExprNode>();

    if (!expr->type.isMatrix()) {
        node->kind = MatrixExprNode::Scalar;
        node->type = expr->type;
        node->scalar = emitExpr(expr);
        return node;
    }

    node->type = expr->type.elementType();
    if (auto* binary = dynamic_cast<BinaryExpr*>(expr)) {
        node->kind = MatrixExprNode::Binary;
        node->op = binary->op;
        node->lhs = planMatrixExpr(binary->left.get(), plan);
        node->rhs = planMatrixExpr(binary->right.get(), plan);
    } else if (auto* unary = dynamic_cast<UnaryExpr*>(expr)) {
        node->kind = MatrixExprNode::Negate;
        node->lhs = planMatrixExpr(unary->operand.get(), plan);
    } else if (auto* cast = dynamic_cast<CastExpr*>(expr)) {
        node->kind = MatrixExprNode::Cast;
        node->fromType = cast->operand->type.elementType();
        node->lhs = planMatrixExpr(cast->operand.get(), plan);
    } else {
        node->kind = MatrixExprNode::Input;
        node->inputIndex = static_cast<unsigned>(plan.inputs.size());
        plan.inputs.push_back(emitExpr(expr));
    }
    return node;
}

// Planı tek bir eleman için hesaplar; linalg.generic'in gövdesinde çağrılır.
mlir::Value MLIRGenerator::evalMatrixExpr(const MatrixExprNode& node, mlir::ValueRange elements,
                                          mlir::Location loc) {
    switch (node.kind) {
        case MatrixExprNode::Input:
            return elements[node.inputIndex];
        case MatrixExprNode::Scalar:
            return node.scalar;
        case MatrixExprNode::Binary:
            // Sema iki tarafı da sonucun eleman tipine getirmiştir.
            return arithManager.buildBinary(node.op, evalMatrixExpr(*node.lhs, elements, loc),
                                            evalMatrixExpr(*node.rhs, elements, loc),
                                            node.type, loc);
        case MatrixExprNode::Negate:
            return arithManager.buildUnary(UnaryExpr::Negate,
                                           evalMatrixExpr(*node.lhs, elements, loc),
                                           node.type, loc);
        case MatrixExprNode::Cast:
            return arithManager.buildCast(evalMatrixExpr(*node.lhs, elements, loc),
                                          node.fromType, node.type, loc);
    }
    return {};
}

void MLIRGenerator::emitMatrixElementwise(Expression* node) {
    auto loc = locOf(node);
    MatrixExprPlan plan;
    auto root = planMatrixExpr(node, plan);

    setMatrixResult(matrixManager.elementwise(
        plan.inputs, typeManager.convert(node->type.elementType()), node->line,
        [&](mlir::ValueRange elements) { return evalMatrixExpr(*root, elements, loc); }, loc));
}

// ===========================================================================
// Literal  [[1, 2], [3, 4]]
// ===========================================================================

// İç içelik derinliği matrisin boyut sayısı kadardır (Sema doğruladı):
// [1, 2] bir vektör, [[1, 2], [3, 4]] bir matris, bir seviye daha üç boyutlu.
void MLIRGenerator::emitMatrixLiteral(ListLiteral* node) {
    auto loc = locOf(node);
    mlir::Type indexType = ctx.builder.getIndexType();
    const int rank = node->type.rank;

    // Şekil: her seviyede ilk alt listenin uzunluğu.
    llvm::SmallVector<mlir::Value, 4> sizes;
    ListLiteral* level = node;
    for (int d = 0; d < rank; ++d) {
        sizes.push_back(arithManager.buildIntConstant(
            static_cast<int64_t>(level->elements.size()), indexType, loc));
        if (d + 1 < rank) level = static_cast<ListLiteral*>(level->elements.front().get());
    }
    // Elemanlar tek tek yazıldığı için literal önce CPU belleğinde kurulur;
    // @GPU fonksiyonda sonra GPU'ya kopyalanır.
    mlir::Value buffer = matrixManager.allocateIn(
        sizes, typeManager.convert(node->type.elementType()), /*onDevice=*/false, loc);

    // Elemanları, o ana kadar inilen indekslerle belleğe yaz.
    llvm::SmallVector<mlir::Value, 4> indices;
    std::function<void(ListLiteral*, int)> store = [&](ListLiteral* list, int depth) {
        for (size_t i = 0; i < list->elements.size(); ++i) {
            indices.push_back(arithManager.buildIntConstant(static_cast<int64_t>(i), indexType, loc));
            if (depth + 1 < rank)
                store(static_cast<ListLiteral*>(list->elements[i].get()), depth + 1);
            else
                ctx.builder.create<mlir::memref::StoreOp>(
                    loc, emitExpr(list->elements[i].get()), buffer, mlir::ValueRange(indices));
            indices.pop_back();
        }
    };
    store(node, 0);

    if (ctx.onDevice) {
        mlir::Value hostBuffer = buffer;
        buffer = matrixManager.upload(hostBuffer, loc);
        matrixManager.free(hostBuffer, /*onDevice=*/false, loc);
    }

    setMatrixResult({matrixManager.toTensor(buffer, /*writable=*/false, loc), buffer});
}

// ===========================================================================
// Yerleşikler
// ===========================================================================

void MLIRGenerator::emitMatrixBuiltin(FunctionCallExpr* node) {
    auto loc = locOf(node);
    auto& args = node->arguments;
    mlir::Type i64 = ctx.builder.getI64Type();

    switch (node->builtin) {
        // zeros(d1, ..., dn) / ones(...) / fill(d1, ..., dn, deger)
        case Builtin::Zeros: case Builtin::Ones: case Builtin::Fill: {
            const SemType& elem = node->type.elementType();
            llvm::SmallVector<mlir::Value, 4> sizes;
            for (int d = 0; d < node->type.rank; ++d)
                sizes.push_back(toIndex(emitExpr(args[d].get()), args[d]->type, loc));

            mlir::Value value;
            if (node->builtin == Builtin::Fill) {
                value = emitExpr(args.back().get());
            } else if (node->builtin == Builtin::Zeros) {
                value = emitZero(elem, loc);
            } else {
                mlir::Type elemType = typeManager.convert(elem);
                value = elem.isFloat()
                    ? arithManager.buildFloatConstant(1.0, mlir::cast<mlir::FloatType>(elemType), loc)
                    : arithManager.buildIntConstant(1, elemType, loc);
            }
            setMatrixResult(matrixManager.filled(sizes, value, loc));
            return;
        }

        // shape(m, i) — rows(m) = shape(m, 0), cols(m) = shape(m, 1)
        case Builtin::Shape: case Builtin::Rows: case Builtin::Cols: {
            unsigned dimension = node->builtin == Builtin::Cols ? 1 : 0;
            if (node->builtin == Builtin::Shape)
                dimension = static_cast<unsigned>(static_cast<IntegerLiteral*>(args[1].get())->value);

            mlir::Value tensor = emitExpr(args[0].get());
            lastValue = ctx.builder.create<mlir::arith::IndexCastOp>(
                loc, i64, matrixManager.dim(tensor, dimension, loc));
            return;
        }

        case Builtin::Transpose:
            setMatrixResult(matrixManager.transpose(emitExpr(args[0].get()), loc));
            return;

        case Builtin::Matmul: {
            mlir::Value lhs = emitExpr(args[0].get());
            mlir::Value rhs = emitExpr(args[1].get());
            setMatrixResult(matrixManager.matmul(lhs, rhs, node->line, loc));
            return;
        }

        // sum(a * b) gibi bir çağrıda argüman da aynı işleme katılır: çarpım
        // için ara matris ayrılmaz, tek döngüde çarpılıp toplanır.
        case Builtin::Sum: {
            MatrixExprPlan plan;
            auto root = planMatrixExpr(args[0].get(), plan);
            lastValue = matrixManager.sum(
                plan.inputs, typeManager.convert(args[0]->type.elementType()), node->line,
                [&](mlir::ValueRange elements) { return evalMatrixExpr(*root, elements, loc); },
                loc);
            return;
        }

        default:
            return;
    }
}

// print(m): son boyut bir çıktı satırıdır (elemanlar boşlukla ayrılır).
//
//   vektör            1 2 3
//   matris            her satır ayrı bir satırda
//   3 ve üstü boyut   iki boyutlu dilimler, aralarında boş satır
//
// Her boyut için bir döngü üretilir; en içteki döngü elemanları yazar.
void MLIRGenerator::emitMatrixPrint(Expression* matrix, mlir::Location loc) {
    mlir::OpBuilder& b = ctx.builder;
    const SemType elemType = matrix->type.elementType();
    const int rank = matrix->type.rank;
    mlir::Value buffer = emitMatrixBuffer(matrix);

    mlir::Value zero = b.create<mlir::arith::ConstantIndexOp>(loc, 0);
    mlir::Value one  = b.create<mlir::arith::ConstantIndexOp>(loc, 1);
    mlir::Value space = b.create<mlir::arith::ConstantIntOp>(loc, ' ', 32);
    mlir::Value newline = b.create<mlir::arith::ConstantIntOp>(loc, '\n', 32);

    // Matrisin bütün satırları tek parça çıksın (bkz. minik_print_begin).
    runtime.call("minik_print_begin", {}, loc);

    llvm::SmallVector<mlir::Value, 4> indices;
    std::function<void(int)> emitDimension = [&](int d) {
        mlir::Value size = b.create<mlir::memref::DimOp>(loc, buffer, static_cast<int64_t>(d));
        b.create<mlir::scf::ForOp>(loc, zero, size, one, mlir::ValueRange(),
            [&](mlir::OpBuilder&, mlir::Location, mlir::Value index, mlir::ValueRange) {
                mlir::Value notFirst = b.create<mlir::arith::CmpIOp>(
                    loc, mlir::arith::CmpIPredicate::ne, index, zero);
                indices.push_back(index);

                if (d == rank - 1) {
                    // Eleman: öncekinden boşlukla ayrılır.
                    runtime.callIf(notFirst, "minik_print_char", {space}, loc);
                    mlir::Value element = b.create<mlir::memref::LoadOp>(
                        loc, buffer, mlir::ValueRange(indices));
                    builtinManager.printValue(element, elemType, loc);
                } else {
                    // İki boyutlu dilimler arasına boş satır.
                    if (d < rank - 2)
                        runtime.callIf(notFirst, "minik_print_char", {newline}, loc);
                    emitDimension(d + 1);
                }

                indices.pop_back();
                b.create<mlir::scf::YieldOp>(loc);
            });
        // Bir satırın elemanları bitti.
        if (d == rank - 1)
            runtime.call("minik_print_char", {newline}, loc);
    };
    emitDimension(0);

    runtime.call("minik_print_end", {}, loc);
}
