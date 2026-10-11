// MLIRGenerator — deyimler

#include "MLIRGenerator.h"

#include "mlir/Dialect/Arith/IR/Arith.h"
#include "mlir/Dialect/MemRef/IR/MemRef.h"

// ===========================================================================
// Bloklar
// ===========================================================================

void MLIRGenerator::emitStatements(const std::vector<std::unique_ptr<Statement>>& statements) {
    for (auto& stmt : statements) {
        stmt->accept(*this);
        // return / break / continue bloğu sonlandırdı: devamı erişilemez.
        if (MLIRControlFlowManager::isTerminated(ctx.builder.getInsertionBlock()))
            break;
    }
}

void MLIRGenerator::visit(BlockStmt* node) {
    scopeManager.pushScope();
    matrixScopes.emplace_back();
    emitStatements(node->statements);

    // Blok bitti: burada tanımlanan matris değişkenlerinin belleğini bırak.
    // (Blok return / break / continue ile bittiyse onlar zaten bıraktı.)
    if (!MLIRControlFlowManager::isTerminated(ctx.builder.getInsertionBlock()))
        freeMatrixScopes(matrixScopes.size() - 1, locOf(node));
    matrixScopes.pop_back();
    scopeManager.popScope();
}

void MLIRGenerator::visit(ExprStmt* node) {
    node->expr->accept(*this);
    flushTemporaries(locOf(node));
}

// ===========================================================================
// Değişkenler
// ===========================================================================

// Yerel değişken tanımı  (varType name = expr;  |  varType name;  |  let name = expr;)
// Globaller Program ziyaretinde emitGlobalVariable ile üretilir.
void MLIRGenerator::visit(VariableDecl* node) {
    auto loc = locOf(node);
    const SemType& type = node->varType;

    // Fonksiyon değerleri yuvaya konmaz; isim doğrudan SSA değerine bağlanır.
    if (type.isFunction()) {
        scopeManager.declareVariable(node->name, emitExpr(node->initializer.get()));
        flushTemporaries(loc);
        return;
    }

    mlir::Value storage = createStorage(type, loc);

    // Matris: yuva belleği tutar. Başlangıç değeri yeni hesaplandıysa belleği
    // devralınır, başka bir değişkense kopyalanır. Değişken, blok bitince
    // bırakılmak üzere kaydedilir.
    if (type.isMatrix()) {
        mlir::Value buffer = takeMatrixOwnership(emitExpr(node->initializer.get()), node->line, loc);
        ctx.builder.create<mlir::memref::StoreOp>(loc, buffer, storage);
        flushTemporaries(loc);
        scopeManager.declareVariable(node->name, storage);
        matrixScopes.back().push_back(storage);
        return;
    }

    auto* list = dynamic_cast<ListLiteral*>(node->initializer.get());
    if (list && type.isArray()) {
        // int[3] a = [1, 2, 3];  → elemanlar geçici dizi olmadan doğrudan yazılır
        emitListInto(list, storage);
    } else if (node->initializer) {
        // (list<T> için değer bir başvurudur: yuvaya yalnızca o yazılır)
        storeTo(emitExpr(node->initializer.get()), storage, type, loc);
    } else if (type.isList()) {
        // list<int> l;  → boş liste
        mlir::Value empty = listManager.create(typeManager.convert(type.elementType()), {}, loc);
        storeTo(empty, storage, type, loc);
    } else if (!type.isArray()) {
        // Başlangıç değeri verilmeyen skaler ve stringler sıfırlanır
        storeTo(emitZero(type, loc), storage, type, loc);
    }

    flushTemporaries(loc);
    scopeManager.declareVariable(node->name, storage);
}

void MLIRGenerator::visit(AssignmentStmt* node) {
    auto loc = locOf(node);
    mlir::Value value = emitExpr(node->value.get());

    if (auto* ident = dynamic_cast<Identifier*>(node->target.get())) {
        mlir::Value storage = ident->ref == Identifier::Ref::Global
            ? emitGlobalRef(ident->name, loc)
            : scopeManager.lookupVariable(ident->name);

        if (ident->type.isMatrix()) {
            // Yeni değer her zaman yeni bir bellektedir (sağ taraf eski değeri
            // kullanmış olabilir: a = a + b). Yuvaya yazılır, eskisi bırakılır.
            mlir::Value newBuffer = takeMatrixOwnership(value, node->line, loc);
            mlir::Value oldBuffer = ctx.builder.create<mlir::memref::LoadOp>(loc, storage);
            ctx.builder.create<mlir::memref::StoreOp>(loc, newBuffer, storage);
            matrixManager.freeBuffer(oldBuffer, loc);
        } else {
            storeTo(value, storage, ident->type, loc);
        }
        flushTemporaries(loc);
        return;
    }

    auto* access = static_cast<ArrayAccessExpr*>(node->target.get());

    // matris[i, j, ...] = deger  — değişkenin kendi belleğine yazılır
    if (access->target->type.isMatrix()) {
        mlir::Value buffer = emitMatrixBuffer(access->target.get());
        llvm::SmallVector<mlir::Value, 4> indices;
        for (auto& index : access->indices)
            indices.push_back(toIndex(emitExpr(index.get()), index->type, loc));
        matrixManager.storeElement(buffer, indices, value, node->line, loc);
        flushTemporaries(loc);
        return;
    }

    // dizi[indeks] = deger
    mlir::Value memref = emitExpr(access->target.get());
    mlir::Value index  = toIndex(emitExpr(access->indices[0].get()), access->indices[0]->type, loc);
    if (access->target->type.isList())
        listManager.storeElement(memref, index, value, node->line, loc); // sınır denetimli
    else
        ctx.builder.create<mlir::memref::StoreOp>(loc, value, memref, mlir::ValueRange(index));
    flushTemporaries(loc);
}

// ===========================================================================
// if / else-if / else
// ===========================================================================

void MLIRGenerator::visit(IfStmt* node) {
    mlir::Value cond = emitExpr(node->condition.get());
    flushTemporaries(locOf(node));
    cfManager.buildIf(locOf(node), cond, /*structured=*/!node->needsCFG,
        [&] { node->trueBlock->accept(*this); },
        buildElseChain(node, 0));
}

// else-if zincirini iç içe if'lere açar: node->elseIfs[idx..] ve elseBlock.
// Else dalı yoksa boş bir std::function döner.
std::function<void()> MLIRGenerator::buildElseChain(IfStmt* node, size_t idx) {
    if (idx >= node->elseIfs.size()) {
        if (!node->elseBlock) return nullptr;
        return [this, node] { node->elseBlock->accept(*this); };
    }
    return [this, node, idx] {
        IfStmt::ElseIf& elseIf = node->elseIfs[idx];
        mlir::Value cond = emitExpr(elseIf.condition.get());
        flushTemporaries(locOf(elseIf.condition.get()));
        cfManager.buildIf(locOf(elseIf.condition.get()), cond, /*structured=*/!node->needsCFG,
            [this, &elseIf] { elseIf.block->accept(*this); },
            buildElseChain(node, idx + 1));
    };
}

// ===========================================================================
// Döngüler
//
// Yapısal (scf) mı bloklu (cf) mu üretileceğini Sema işaretler; ayrıntılar
// MLIRControlFlowManager'dadır.
// ===========================================================================

void MLIRGenerator::visit(WhileStmt* node) {
    cfManager.buildWhile(locOf(node), /*structured=*/!node->needsCFG, node->hasContinue,
        [&] {
            mlir::Value cond = emitExpr(node->condition.get());
            flushTemporaries(locOf(node)); // koşul her yinelemede yeniden hesaplanır
            return cond;
        },
        [&] { emitLoopBody(node->body.get()); });
}

// Döngü gövdesi. break/continue'nun hangi blokların matris değişkenlerini
// bırakacağını bilmesi için gövdenin başladığı derinlik kaydedilir.
void MLIRGenerator::emitLoopBody(BlockStmt* body) {
    loopScopeDepths.push_back(matrixScopes.size());
    body->accept(*this);
    loopScopeDepths.pop_back();
}

// for i in start .. end step s
// Sınırlar döngüden önce bir kez hesaplanır. Döngü değişkeni salt okunur bir
// SSA değeridir (bellekte yuvası yoktur).
void MLIRGenerator::visit(ForRangeStmt* node) {
    auto loc = locOf(node);
    mlir::Value start = emitExpr(node->start.get());
    mlir::Value end   = emitExpr(node->end.get());
    mlir::Value step  = node->step
        ? emitExpr(node->step.get())
        : arithManager.buildIntConstant(1, typeManager.convert(node->iterType), loc);
    flushTemporaries(loc);

    if (node->isParallel) {
        emitParallelFor(node, start, end, step);
        return;
    }

    cfManager.buildCountedLoop(loc, start, end, step, node->iterType.isSigned,
        /*structured=*/!node->needsCFG, node->hasContinue,
        [&](mlir::Value iv) {
            scopeManager.pushScope();
            scopeManager.declareVariable(node->iteratorName, iv);
            emitLoopBody(node->body.get());
            scopeManager.popScope();
        });
}

// parallel for i in start .. end step s
//
// scf.parallel sınırlarını index tipinde ister; döngü değişkeni gövdede
// yeniden kendi tamsayı tipine çevrilir.
void MLIRGenerator::emitParallelFor(ForRangeStmt* node, mlir::Value start, mlir::Value end,
                                    mlir::Value step) {
    auto loc = locOf(node);
    const SemType& iterType = node->iterType;
    // @GPU fonksiyonda döngü GPU'da çalışır; aksi halde iş parçacıklarında.
    const bool onDevice = ctx.onDevice;
    if (!onDevice) usesParallel = true;

    cfManager.buildParallelLoop(
        loc, toIndex(start, iterType, loc), toIndex(end, iterType, loc),
        toIndex(step, iterType, loc), onDevice,
        [&](mlir::Value index) {
            const bool savedInKernel = ctx.inKernel;
            ctx.inKernel = onDevice;
            // Gövdenin yığıt tahsisleri fonksiyon girişine DEĞİL buraya, her
            // yinelemeye özel bloğa yapılır: iş parçacıkları yerel
            // değişkenleri paylaşmamalı.
            mlir::Block* savedAllocaBlock = ctx.allocaBlock;
            ctx.allocaBlock = ctx.builder.getInsertionBlock();

            mlir::Type mlirIterType = typeManager.convert(iterType);
            mlir::Value iv = iterType.isSigned
                ? ctx.builder.create<mlir::arith::IndexCastOp>(loc, mlirIterType, index).getResult()
                : ctx.builder.create<mlir::arith::IndexCastUIOp>(loc, mlirIterType, index).getResult();

            scopeManager.pushScope();
            scopeManager.declareVariable(node->iteratorName, iv);
            emitLoopBody(node->body.get());
            scopeManager.popScope();

            ctx.allocaBlock = savedAllocaBlock;
            ctx.inKernel = savedInKernel;
        });
}

// for x in dizi  →  0 .. N arası indeks döngüsü; x her yinelemede okunan eleman.
void MLIRGenerator::visit(ForCollectionStmt* node) {
    auto loc = locOf(node);
    mlir::Value collection = emitExpr(node->collection.get());
    flushTemporaries(loc);

    // Listenin uzunluğu döngüye girerken bir kez okunur. Eleman ise her
    // yinelemede listeden (sınır denetimiyle) okunur; böylece gövde listeyi
    // değiştirse bile geçersiz belleğe erişilmez.
    const bool isList = node->collection->type.isList();
    mlir::Type indexType = ctx.builder.getIndexType();
    mlir::Value zero = arithManager.buildIntConstant(0, indexType, loc);
    mlir::Value size = isList
        ? listManager.length(collection, loc)
        : arithManager.buildIntConstant(node->collection->type.arraySize, indexType, loc);
    mlir::Value one  = arithManager.buildIntConstant(1, indexType, loc);

    cfManager.buildCountedLoop(loc, zero, size, one, /*isSigned=*/true,
        /*structured=*/!node->needsCFG, node->hasContinue,
        [&](mlir::Value index) {
            mlir::Value elem = isList
                ? listManager.loadElement(collection, index, node->line, loc)
                : ctx.builder.create<mlir::memref::LoadOp>(
                      loc, collection, mlir::ValueRange(index)).getResult();
            scopeManager.pushScope();
            scopeManager.declareVariable(node->iteratorName, elem);
            emitLoopBody(node->body.get());
            scopeManager.popScope();
        });
}

// break / continue: döngü gövdesinden çıkılırken, gövdede (ve içindeki
// bloklarda) tanımlanmış matris değişkenleri bırakılır.

void MLIRGenerator::visit(BreakStmt* node) {
    freeMatrixScopes(loopScopeDepths.back(), locOf(node));
    cfManager.emitBreak(locOf(node));
}

void MLIRGenerator::visit(ContinueStmt* node) {
    freeMatrixScopes(loopScopeDepths.back(), locOf(node));
    cfManager.emitContinue(locOf(node));
}

void MLIRGenerator::visit(ReturnStmt* node) {
    auto loc = locOf(node);
    if (!node->expr) {
        freeMatrixScopes(0, loc);
        ctx.builder.create<mlir::func::ReturnOp>(loc, mlir::ValueRange());
        return;
    }

    // Fonksiyondan çıkılırken bütün yerel matrisler bırakılır. Dönen değer
    // bir matrisse belleğinin sahipliği çağırana geçer:
    //   * yerel bir değişkense belleği kopyalanmadan taşınır (o bırakılmaz),
    //   * yeni hesaplanmış bir geçiciyse olduğu gibi döner,
    //   * bir parametreyse (çağıranın belleği) kopyası döner.
    mlir::Value movedSlot;
    mlir::Value value;
    if (node->expr->type.isMatrix()) {
        movedSlot = ownedMatrixSlot(node->expr.get());
        value = movedSlot
            ? ctx.builder.create<mlir::memref::LoadOp>(loc, movedSlot).getResult()
            : takeMatrixOwnership(emitExpr(node->expr.get()), node->line, loc);
    } else {
        value = emitExpr(node->expr.get());
    }

    flushTemporaries(loc);
    freeMatrixScopes(0, loc, movedSlot);
    ctx.builder.create<mlir::func::ReturnOp>(loc, mlir::ValueRange(value));
}
