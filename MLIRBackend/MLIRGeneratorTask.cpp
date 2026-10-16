// MLIRGenerator — görevler: marl, await, coroutine fonksiyonlar
//
// marl f(a, b)
//
//   Argümanlar çağıranda hesaplanır ve göreve ait kopyalara çevrilir (bkz.
//   Sema.h, görev kuralı 1). Görevin gövdesi (çağrı + kopyaların
//   bırakılması) ayrı bir fonksiyondur; async.execute onu çağırır. MLIR
//   async.execute bölgesini bir coroutine'e taşır ve iş parçacığı havuzunda
//   çalıştırır:
//
//     func @__marl_0(%a, %b) -> i64 {     // f coroutine ise bu da async.func
//       %r = func.call @f(%a, %b)
//       <kopyaları bırak>
//       return %r : i64
//     }
//     %a2 = <a'nın kopyası>
//     %token, %value = async.execute -> !async.value<i64> {
//       %r = func.call @__marl_0(%a2, %b)
//       async.yield %r : i64
//     }
//     %t = call @marlin_task_wrap.0(%value, %isString)   // → handle (i64)
//
//   Görevin değeri, runtime'daki başvuru sayılan bir handle'dır (bkz.
//   runtime/marlin_task.c). Sahiplik kuralları string ile aynıdır
//   (MLIRGeneratorString.cpp).
//
// await(t)
//
//     %v = call @marlin_task_value.0(%t) : (i64) -> !async.value<i64>
//     %r = async.await %v : !async.value<i64>
//
//   async.await, bir coroutine'in (async.func ya da görev gövdesi) içindeyse
//   coroutine'i askıya alır: iş parçacığı başka işe geçer, görev bitince
//   kalınan yerden devam edilir. Sıradan bir fonksiyonda (ör. main) iş
//   parçacığını bekletir.
//
// COROUTINE FONKSİYONLAR
//
//   Sema'nın işaretlediği fonksiyonlar (FunctionDecl::isAsync: görevden
//   ulaşılan ve bekleyebilen) async.func olarak üretilir; çağrıları
//   async.call + async.await olur (bkz. emitCallOp). Gerisi sıradan fonksiyon
//   üretimiyle aynıdır.
//
// TİPLİ KÖPRÜLER
//
//   Runtime fonksiyonu !async.value'yu doğrudan argüman olarak alır ve döndürür:
//   böylece MLIR'in başvuru sayımı (async-runtime-ref-counting) onu bir
//   fonksiyon çağrısına verilen değer olarak tanır ve handle'a geçen başvuruyu
//   doğru sayar. !async.value<T> her T için ayrı bir MLIR tipidir; bu yüzden
//   her sonuç tipi için ayrı adla bildirilir (marlin_task_wrap.0,
//   marlin_task_wrap.1 ...). LLVM'e indikten sonra hepsi aynı işaretçi tipini
//   alır ve tek C fonksiyonuna bağlanır (bkz. LLVMLowering.cpp).

#include "MLIRGenerator.h"

#include "mlir/Dialect/Arith/IR/Arith.h"
#include "mlir/Dialect/Async/IR/Async.h"
#include "mlir/Dialect/MemRef/IR/MemRef.h"
#include "mlir/Dialect/SCF/IR/SCF.h"

std::string MLIRGenerator::taskBridge(llvm::StringRef base, mlir::Type asyncType) {
    auto [entry, inserted] = taskBridgeIds.try_emplace(asyncType, taskBridgeIds.size());
    return (base + "." + std::to_string(entry->second)).str();
}

// ===========================================================================
// marl
// ===========================================================================

void MLIRGenerator::visit(MarlExpr* node) {
    auto loc = locOf(node);
    auto* call = static_cast<FunctionCallExpr*>(node->call.get()); // Sema doğruladı
    const SemType& resultType = call->type;
    usesTasks = true;

    // 1) Argümanlar burada, çağıranda hesaplanır ve kopyalanır.
    auto* ident = dynamic_cast<Identifier*>(call->target.get());
    const bool isDirect = ident && ident->ref == Identifier::Ref::Function;
    mlir::Value calleeValue;
    if (!isDirect) calleeValue = emitExpr(call->target.get()); // fonksiyonun adresi (i64)

    llvm::SmallVector<mlir::Value, 4> args;
    llvm::SmallVector<Owned, 4> copies;
    for (auto& argExpr : call->arguments) {
        Values values = argExpr->type.isComposite() ? emitLeaves(argExpr.get())
                                                    : Values{emitExpr(argExpr.get())};
        const std::vector<SemLeaf> leaves = leavesOf(argExpr->type);
        for (size_t i = 0; i < leaves.size(); ++i) {
            mlir::Value copy = copyForTask(values[i], leaves[i].type, call->line, loc);
            args.push_back(copy);
            copies.push_back({copy, leaves[i].type});
        }
        // Liste literali: görev kendi kopyasını aldı, aslı bu deyimde biter.
        if (argExpr->type.isList() && dynamic_cast<ListLiteral*>(argExpr.get()))
            ownTemporaryList(argExpr.get(), values);
    }

    // 2) Görevin gövdesi ayrı bir fonksiyondur (__marl_N): çağrı + kopyaların
    //    bırakılması. async.execute bölgesi tek bloklu olmalıdır ve indirme
    //    hattında scf → cf dönüşümü coroutine'lerden önce gelir (bkz.
    //    LLVMLowering.cpp); döngüler ve koşullar bu yüzden bölgenin içinde
    //    değil bu fonksiyonda durur. Çağrılan fonksiyon coroutine ise gövde de
    //    coroutine olur (beklerken iş parçacığını bırakması için).
    const bool calleeIsAsync = isDirect && functions.at(ident->name)->isAsync;
    llvm::SmallVector<mlir::Value, 5> bodyArgs(args.begin(), args.end());
    if (calleeValue) bodyArgs.push_back(calleeValue);
    llvm::SmallVector<mlir::Type, 5> bodyInputs;
    for (mlir::Value arg : bodyArgs) bodyInputs.push_back(arg.getType());
    llvm::SmallVector<mlir::Type, 1> resultTypes;
    if (!resultType.isVoid()) resultTypes.push_back(typeManager.convert(resultType));

    const std::string bodyName = "__marl_" + std::to_string(taskBodyCounter++);
    auto plainType = ctx.builder.getFunctionType(bodyInputs, resultTypes);
    mlir::FunctionOpInterface body;
    {
        mlir::OpBuilder::InsertionGuard guard(ctx.builder);
        ctx.builder.setInsertionPointToEnd(ctx.theModule.getBody());
        if (calleeIsAsync) {
            llvm::SmallVector<mlir::Type, 1> asyncResults;
            if (resultTypes.empty()) asyncResults.push_back(mlir::async::TokenType::get(&ctx.context));
            for (mlir::Type t : resultTypes) asyncResults.push_back(mlir::async::ValueType::get(t));
            auto op = ctx.builder.create<mlir::async::FuncOp>(
                loc, bodyName, ctx.builder.getFunctionType(bodyInputs, asyncResults));
            op.setPrivate();
            body = op;
        } else {
            auto op = ctx.builder.create<mlir::func::FuncOp>(loc, bodyName, plainType);
            op.setPrivate();
            body = op;
        }

        mlir::Block* entry = body.addEntryBlock();
        ctx.builder.setInsertionPointToEnd(entry);
        mlir::Block* savedAllocaBlock = ctx.allocaBlock;
        const bool savedAsync = emittingAsync;
        const bool savedMain = emittingMain;
        emittingMain = false;
        const bool savedHeapLocals = ctx.heapLocals;
        auto savedHeapLocalValues = std::move(ctx.heapLocalValues);
        ctx.allocaBlock = entry;
        emittingAsync = calleeIsAsync;
        ctx.heapLocals = calleeIsAsync;
        ctx.heapLocalValues.clear();

        llvm::SmallVector<mlir::Value, 4> params(entry->getArguments().begin(),
                                                 entry->getArguments().begin() + args.size());
        mlir::Value calleeParam = calleeValue ? entry->getArguments().back() : mlir::Value{};
        Values results = emitCallOp(call, calleeParam, params, loc);
        for (size_t i = 0; i < copies.size(); ++i) {
            const SemType& type = copies[i].type;
            mlir::Value copy = params[i];
            // Görev, kendisine verilen listeyi aynen döndürebilir
            // (func f(list<int> l) -> list<int> { return l; }): o liste
            // artık sonucun kendisidir, bırakılmaz.
            if (type.isList() && resultType.isList() && type.elementType() == resultType.elementType()) {
                auto pointerOf = [&](mlir::Value cell) {
                    return ctx.builder.create<mlir::memref::ExtractAlignedPointerAsIndexOp>(loc, cell);
                };
                mlir::Value differs = ctx.builder.create<mlir::arith::CmpIOp>(
                    loc, mlir::arith::CmpIPredicate::ne, pointerOf(copy), pointerOf(results[0]));
                runtime.ifThen(differs, loc, [&] { releaseTaskCopy(copy, type, loc); });
                continue;
            }
            releaseTaskCopy(copy, type, loc);
        }
        emitReturnOp(results, loc);

        ctx.heapLocals = savedHeapLocals;
        ctx.heapLocalValues = std::move(savedHeapLocalValues);
        emittingAsync = savedAsync;
        emittingMain = savedMain;
        ctx.allocaBlock = savedAllocaBlock;
    }

    auto execute = ctx.builder.create<mlir::async::ExecuteOp>(
        loc, resultTypes, mlir::ValueRange(), mlir::ValueRange(),
        [&](mlir::OpBuilder& b, mlir::Location l, mlir::ValueRange) {
            llvm::SmallVector<mlir::Value, 1> results;
            if (calleeIsAsync) {
                auto bodyCall = b.create<mlir::async::CallOp>(
                    l, mlir::cast<mlir::async::FuncOp>(body.getOperation()), mlir::ValueRange(bodyArgs));
                auto await = b.create<mlir::async::AwaitOp>(l, bodyCall.getResult(0));
                if (await.getResult()) results.push_back(await.getResult());
            } else {
                auto bodyCall = b.create<mlir::func::CallOp>(
                    l, mlir::cast<mlir::func::FuncOp>(body.getOperation()), mlir::ValueRange(bodyArgs));
                results.append(bodyCall.getResults().begin(), bodyCall.getResults().end());
            }
            b.create<mlir::async::YieldOp>(l, mlir::ValueRange(results));
        });

    // 3) Handle.
    mlir::Type i64 = ctx.builder.getI64Type();
    mlir::Value handle;
    if (resultType.isVoid()) {
        handle = runtime.callValue("marlin_task_wrap_token", {execute.getToken()}, i64, loc);
    } else {
        mlir::Value value = execute.getBodyResults()[0];
        mlir::Value isString = arithManager.buildIntConstant(resultType.isString() ? 1 : 0,
                                                             ctx.builder.getI32Type(), loc);
        handle = runtime.callValue(taskBridge("marlin_task_wrap", value.getType()),
                                   {value, isString}, i64, loc);
    }
    lastValue = handleResult(handle, node->type);
}

// ===========================================================================
// Görevin argümanları
// ===========================================================================

mlir::Value MLIRGenerator::copyForTask(mlir::Value value, const SemType& type, int line,
                                       mlir::Location loc) {
    if (isHandle(type)) {
        retainHandle(value, type, loc);
        return value;
    }
    if (type.isMatrix()) return takeMatrixOwnership(value, line, loc);
    if (type.isArray()) {
        // Dizi başlatanın yığıtında yaşar; görev ondan uzun yaşayabilir.
        auto memRefType = mlir::cast<mlir::MemRefType>(typeManager.convert(type));
        mlir::Value copy = ctx.builder.create<mlir::memref::AllocOp>(loc, memRefType);
        copyArray(value, copy, type, /*releaseOld=*/false, loc);
        return copy;
    }
    if (type.isList()) return cloneList(value, type, loc);
    return value; // sayı, bool, char, fonksiyon
}

void MLIRGenerator::releaseTaskCopy(mlir::Value copy, const SemType& type, mlir::Location loc) {
    if (isHandle(type)) {
        releaseHandle(copy, type, loc);
    } else if (type.isMatrix()) {
        matrixManager.freeBuffer(copy, loc);
    } else if (type.isArray()) {
        if (holdsStrings(type)) releaseStringArray(copy, type, loc);
        ctx.builder.create<mlir::memref::DeallocOp>(loc, copy);
    } else if (type.isList()) {
        const SemType& elem = type.elementType();
        if (isHandle(elem)) {
            mlir::Value zero = ctx.builder.create<mlir::arith::ConstantIndexOp>(loc, 0);
            mlir::Value one = ctx.builder.create<mlir::arith::ConstantIndexOp>(loc, 1);
            mlir::Value length = listManager.length(copy, loc);
            ctx.builder.create<mlir::scf::ForOp>(
                loc, zero, length, one, mlir::ValueRange(),
                [&](mlir::OpBuilder& b, mlir::Location l, mlir::Value index, mlir::ValueRange) {
                    releaseHandle(listManager.loadElement(copy, index, 0, l, /*checked=*/false), elem, l);
                    b.create<mlir::scf::YieldOp>(l);
                });
        }
        listManager.free(copy, loc);
    }
}

// Elemanları tek değerlik (sayı, string, fonksiyon, görev) bir listenin kopyası.
mlir::Value MLIRGenerator::cloneList(mlir::Value list, const SemType& listType, mlir::Location loc) {
    const SemType& elem = listType.elementType();
    mlir::Value copy = listManager.create(typeManager.convert(elem), {}, loc);
    mlir::Value zero = ctx.builder.create<mlir::arith::ConstantIndexOp>(loc, 0);
    mlir::Value one = ctx.builder.create<mlir::arith::ConstantIndexOp>(loc, 1);
    mlir::Value length = listManager.length(list, loc);
    ctx.builder.create<mlir::scf::ForOp>(
        loc, zero, length, one, mlir::ValueRange(),
        [&](mlir::OpBuilder& b, mlir::Location l, mlir::Value index, mlir::ValueRange) {
            mlir::Value value = listManager.loadElement(list, index, 0, l, /*checked=*/false);
            if (isHandle(elem)) retainHandle(value, elem, l);
            listManager.push(copy, value, l);
            b.create<mlir::scf::YieldOp>(l);
        });
    return copy;
}

// ===========================================================================
// await
// ===========================================================================

void MLIRGenerator::emitAwait(FunctionCallExpr* node) {
    auto loc = locOf(node);
    mlir::Value handle = emitExpr(node->arguments[0].get());
    const SemType& resultType = node->type;

    if (resultType.isVoid()) {
        mlir::Value token = runtime.callValue(
            "marlin_task_token", {handle}, mlir::async::TokenType::get(&ctx.context), loc);
        ctx.builder.create<mlir::async::AwaitOp>(loc, token);
        lastValue = mlir::Value{};
        return;
    }

    auto asyncType = mlir::async::ValueType::get(typeManager.convert(resultType));
    mlir::Value value = runtime.callValue(taskBridge("marlin_task_value", asyncType), {handle},
                                          asyncType, loc);
    mlir::Value result = ctx.builder.create<mlir::async::AwaitOp>(loc, value).getResult();
    // Sonuç görevin içinde kalır (başka await'ler de okuyabilir); string ise
    // bu deyim kendi başvurusunu alır.
    if (isHandle(resultType)) {
        retainHandle(result, resultType, loc);
        handleResult(result, resultType);
    }
    lastValue = result;
}
