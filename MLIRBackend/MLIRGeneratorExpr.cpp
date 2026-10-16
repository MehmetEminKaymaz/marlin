// MLIRGenerator — ifadeler

#include "MLIRGenerator.h"

#include "llvm/ADT/SmallVector.h"
#include "mlir/Dialect/Arith/IR/Arith.h"
#include "mlir/Dialect/Async/IR/Async.h"
#include "mlir/Dialect/LLVMIR/LLVMDialect.h"
#include "mlir/Dialect/MemRef/IR/MemRef.h"
#include "mlir/IR/BuiltinAttributes.h"
#include "mlir/IR/BuiltinTypes.h"

// ===========================================================================
// Sabitler
//
// Literalin tipini Sema belirler: `int64 x = 5` içindeki 5 doğrudan i64,
// `float y = 3` içindeki 3 doğrudan f32 sabiti olarak üretilir.
// ===========================================================================

void MLIRGenerator::visit(IntegerLiteral* node) {
    mlir::Type type = typeManager.convert(node->type);
    if (auto floatType = mlir::dyn_cast<mlir::FloatType>(type))
        lastValue = arithManager.buildFloatConstant(
            static_cast<double>(node->value), floatType, locOf(node));
    else
        lastValue = arithManager.buildIntConstant(node->value, type, locOf(node));
}

void MLIRGenerator::visit(FloatLiteral* node) {
    auto floatType = mlir::cast<mlir::FloatType>(typeManager.convert(node->type));
    lastValue = arithManager.buildFloatConstant(node->value, floatType, locOf(node));
}

void MLIRGenerator::visit(BooleanLiteral* node) {
    lastValue = arithManager.buildIntConstant(
        node->value ? 1 : 0, ctx.builder.getI1Type(), locOf(node));
}

void MLIRGenerator::visit(CharLiteral* node) {
    lastValue = arithManager.buildIntConstant(
        static_cast<unsigned char>(node->value), ctx.builder.getIntegerType(8), locOf(node));
}

// String literali, çalışma zamanındaki string bloğuyla aynı düzende bir global
// sabittir; ifadenin değeri o globalin adresidir (i64 olarak):
//
//   [ refs = -1 (8 bayt) | uzunluk (8 bayt) | baytlar ... | \0 ]
//
// refs = -1 "literal" demektir: çalışma zamanı böyle bir stringi asla
// bırakmaz. Düzen runtime/marlin_rt.c'deki MarlinString ile aynı olmalıdır.
// Başlık bayt dizisine küçük sonlu (little-endian) yazılır; desteklenen
// hedef (x86-64) böyledir.
void MLIRGenerator::visit(StringLiteral* node) {
    auto loc = locOf(node);
    std::string globalName = "__str_" + std::to_string(stringLiteralCount++);

    std::string bytes;
    auto appendI64 = [&bytes](int64_t value) {
        for (int i = 0; i < 8; ++i)
            bytes += static_cast<char>((static_cast<uint64_t>(value) >> (8 * i)) & 0xFF);
    };
    appendI64(-1);
    appendI64(static_cast<int64_t>(node->value.size()));
    bytes += node->value;
    bytes += '\0';

    {
        mlir::OpBuilder::InsertionGuard guard(ctx.builder);
        ctx.builder.setInsertionPointToStart(ctx.theModule.getBody());

        auto blockType = mlir::LLVM::LLVMArrayType::get(
            ctx.builder.getIntegerType(8), static_cast<unsigned>(bytes.size()));

        // Başlıktaki 64 bitlik alanlar hizalı okunabilsin diye 8'e hizalanır.
        ctx.builder.create<mlir::LLVM::GlobalOp>(
            loc, blockType, /*isConst=*/true,
            mlir::LLVM::Linkage::Private,
            globalName,
            ctx.builder.getStringAttr(bytes),
            /*alignment=*/8);
    }

    auto ptrType = mlir::LLVM::LLVMPointerType::get(&ctx.context);
    mlir::Value address = ctx.builder.create<mlir::LLVM::AddressOfOp>(loc, ptrType, globalName);
    lastValue = ctx.builder.create<mlir::LLVM::PtrToIntOp>(loc, ctx.builder.getI64Type(), address);
}

// ===========================================================================
// İsimler ve diziler
// ===========================================================================

void MLIRGenerator::visit(Identifier* node) {
    auto loc = locOf(node);

    // Struct (ve struct listesi) tek bir değer değildir; değerini isteyenler
    // emitLeaves kullanır. Buraya yalnızca değeri kullanılmayan bir deyimden
    // gelinir (p;).
    if (node->type.isComposite()) {
        lastValue = mlir::Value{};
        return;
    }

    switch (node->ref) {
        case Identifier::Ref::Function:
            // Fonksiyonun değer olarak kullanımı (ör. değişkene atama, argüman)
            lastValue = functionAddress(node->name, node->type, loc);
            return;

        case Identifier::Ref::Global: {
            mlir::Value globalRef = emitGlobalRef(node->name, loc);
            lastValue = node->type.isArray()
                ? globalRef
                : ctx.builder.create<mlir::memref::LoadOp>(loc, globalRef).getResult();
            return;
        }

        case Identifier::Ref::Local:
        case Identifier::Ref::Unresolved:
            lastValue = loadFrom(scopeManager.lookupVariable(node->name), node->type, loc);
            // Matris, ifade içinde tensor olarak görülür (salt okunur: bellek
            // değişkene aittir).
            if (node->type.isMatrix())
                lastValue = matrixManager.toTensor(lastValue, /*writable=*/false, loc);
            return;
    }
}

// [e0, e1, ...] — Sema'nın verdiği tipe göre yeni bir matris (matrix<T>), yığın
// belleğinde yeni bir liste (list<T>) ya da yığıtta geçici bir dizi (T[N]) oluşturur.
void MLIRGenerator::visit(ListLiteral* node) {
    if (node->type.isMatrix()) {
        emitMatrixLiteral(node);
        return;
    }
    if (node->type.isComposite()) { // struct listesi; değeri emitLeaves ile alınır
        emitLeaves(node);
        lastValue = mlir::Value{};
        return;
    }
    if (node->type.isList()) {
        lastValue = emitStructListLiteral(node).front();
        return;
    }

    // Geçici dizi. String dizisinin elemanları diziye aittir; deyim bitince
    // bırakılırlar.
    mlir::Value memref = createStorage(node->type, locOf(node));
    emitListInto(node, memref);
    if (holdsStrings(node->type)) temporaries.push_back({memref, node->type});
    lastValue = memref;
}
void MLIRGenerator::visit(ArrayAccessExpr* node) {
    auto loc = locOf(node);

    // liste[i], elemanı struct: tek bir değeri yoktur (bkz. visit(Identifier)).
    if (node->type.isComposite()) {
        emitLeaves(node);
        lastValue = mlir::Value{};
        return;
    }

    // matris[i, j, ...] — her boyut için bir indeks; bellek üzerinden, sınır denetimli
    if (node->target->type.isMatrix()) {
        mlir::Value buffer = emitMatrixBuffer(node->target.get());
        llvm::SmallVector<mlir::Value, 4> indices;
        for (auto& index : node->indices)
            indices.push_back(toIndex(emitExpr(index.get()), index->type, loc));
        lastValue = matrixManager.loadElement(buffer, indices, node->line, loc);
        return;
    }

    // string[i] — i. bayt; sınır denetimi çalışma zamanı kütüphanesinde
    if (node->target->type.isString()) {
        mlir::Value text = emitExpr(node->target.get());
        const SemType& indexType = node->indices[0]->type;
        mlir::Value index = emitExpr(node->indices[0].get());
        if (indexType.width != 64)
            index = arithManager.buildCast(index, indexType, SemType::Int(64, indexType.isSigned), loc);
        mlir::Value line = arithManager.buildIntConstant(node->line, ctx.builder.getI32Type(), loc);
        mlir::Value byte = runtime.callValue("marlin_str_at", {text, index, line},
                                             ctx.builder.getI32Type(), loc);
        lastValue = ctx.builder.create<mlir::arith::TruncIOp>(loc, ctx.builder.getIntegerType(8), byte);
        return;
    }

    // liste[i] — sınır denetimli
    if (node->target->type.isList()) {
        lastValue = emitElement(node, 0, 1).front();
        return;
    }

    mlir::Value memref = emitExpr(node->target.get());
    mlir::Value index  = toIndex(emitExpr(node->indices[0].get()), node->indices[0]->type, loc);
    lastValue = ctx.builder.create<mlir::memref::LoadOp>(loc, memref, mlir::ValueRange(index));
    // Okunan string bu deyimin başvurusu olur (bkz. MLIRGeneratorString.cpp).
    if (node->type.isString()) {
        retainString(lastValue, loc);
        stringResult(lastValue);
    }
}

// ===========================================================================
// Çağrılar
// ===========================================================================

// Hedef doğrudan bir fonksiyon ismiyse func.call, aksi halde (lambda değişkeni,
// fonksiyon döndüren ifade...) func.call_indirect üretilir. Argümanlar Sema
// tarafından parametre tiplerine getirilmiştir.
void MLIRGenerator::visit(FunctionCallExpr* node) {
    auto loc = locOf(node);

    // Yerleşik fonksiyon (print, sqrt, push ...): hedef bir fonksiyon değeri değildir.
    if (node->builtin != Builtin::None) {
        emitBuiltinCall(node);
        return;
    }

    // Struct oluşturma ya da struct döndüren çağrı, değeri kullanılmadan
    // (deyim olarak): yine de üretilir, sonucu deyim sonunda bırakılır.
    if (node->type.isComposite()) {
        emitLeaves(node);
        lastValue = mlir::Value{};
        return;
    }

    Values results = emitCall(node);
    lastValue = results.empty() ? mlir::Value{} : results.front();

    // Dönen matris yeni bir bellektir ve artık bu deyime aittir.
    if (node->type.isMatrix())
        setMatrixResult({matrixManager.toTensor(lastValue, /*writable=*/false, loc), lastValue});
}

MLIRGenerator::Values MLIRGenerator::emitCall(FunctionCallExpr* node) {
    auto loc = locOf(node);
    auto* ident = dynamic_cast<Identifier*>(node->target.get());
    const bool isDirect = ident && ident->ref == Identifier::Ref::Function;
    if (isDirect && functions.at(ident->name)->isExtern)
        return emitExternCall(functions.at(ident->name), node);

    mlir::Value calleeValue;
    if (!isDirect) calleeValue = emitExpr(node->target.get());

    // Matris argümanı belleğiyle verilir; kopyalanmaz (çağrılan onu
    // değiştiremez). Argüman bir geçiciyse deyim sonunda bırakılır.
    // Struct argüman yaprak yaprak verilir; her yaprak kendi tipinin kuralıyla.
    llvm::SmallVector<mlir::Value, 4> args;
    for (auto& argExpr : node->arguments) {
        auto* literal = dynamic_cast<ListLiteral*>(argExpr.get());
        if (argExpr->type.isComposite()) {
            Values values = emitLeaves(argExpr.get());
            const std::vector<SemLeaf> leaves = leavesOf(argExpr->type);
            for (size_t i = 0; i < leaves.size(); ++i)
                args.push_back(leaves[i].type.isMatrix() ? matrixManager.toBuffer(values[i], loc)
                                                         : values[i]);
            if (literal && literal->freeAfterCall) ownTemporaryList(literal, values);
            continue;
        }
        mlir::Value arg = emitExpr(argExpr.get());
        if (argExpr->type.isMatrix()) arg = matrixManager.toBuffer(arg, loc);
        if (literal && literal->freeAfterCall) ownTemporaryList(literal, {arg});
        args.push_back(arg);
    }

    Values results = emitCallOp(node, calleeValue, args, loc);
    const std::vector<SemLeaf> resultLeaves = leavesOf(node->type);
    for (size_t i = 0; i < resultLeaves.size(); ++i) {
        const SemType& type = resultLeaves[i].type;
        // Dönen string ve görevlerin başvurusu artık bu deyimindir.
        if (isHandle(type)) handleResult(results[i], type);
        else if (node->type.isStruct() && type.isArray() && holdsStrings(type))
            temporaries.push_back({results[i], type});
    }
    return results;
}

MLIRGenerator::Values MLIRGenerator::emitCallOp(FunctionCallExpr* node, mlir::Value calleeValue,
                                                llvm::ArrayRef<mlir::Value> argValues,
                                                mlir::Location loc) {
    llvm::SmallVector<mlir::Value, 4> args(argValues.begin(), argValues.end());

    // Dönen struct'ın dizi alanları için yeri çağıran ayırır ve son
    // parametreler olarak verir (bkz. MLIRTypeManager::convertFunction).
    const std::vector<SemLeaf> resultLeaves = leavesOf(node->type);
    Values arraySlots;
    if (node->type.isStruct()) {
        for (const SemLeaf& leaf : resultLeaves) {
            if (!leaf.type.isArray()) continue;
            arraySlots.push_back(createStorage(leaf.type, loc));
            args.push_back(arraySlots.back());
        }
    }

    llvm::SmallVector<mlir::Value, 4> callResults;
    auto* ident = dynamic_cast<Identifier*>(node->target.get());
    if (!calleeValue) {
        FunctionDecl* decl = functions.at(ident->name);
        if (decl->isAsync) {
            // Coroutine fonksiyon: async.call sonucu bir görev değeridir;
            // hemen beklenir. Çağıran da coroutine ise bekleme onu askıya
            // alır (iş parçacığını bırakır), değilse iş parçacığını bekletir.
            auto callee = ctx.theModule.lookupSymbol<mlir::async::FuncOp>(ident->name);
            auto call = ctx.builder.create<mlir::async::CallOp>(loc, callee, mlir::ValueRange(args));
            for (mlir::Value result : call.getResults()) {
                auto await = ctx.builder.create<mlir::async::AwaitOp>(loc, result);
                if (await.getResult()) callResults.push_back(await.getResult());
            }
        } else {
            // CPU'dan, matris alan/döndüren bir @GPU fonksiyona çağrı: veriyi
            // GPU'ya (ve sonucu geri) kopyalayan giriş fonksiyonu çağrılır.
            const bool crossesToGpu = decl->isGpu && ctx.gpuEnabled && !ctx.onDevice &&
                                      decl->hasMatrixSignature();
            auto callee = ctx.theModule.lookupSymbol<mlir::func::FuncOp>(
                crossesToGpu ? hostEntryName(ident->name) : ident->name);
            auto call = ctx.builder.create<mlir::func::CallOp>(loc, callee, mlir::ValueRange(args));
            callResults.append(call.getResults().begin(), call.getResults().end());
        }
    } else {
        // Fonksiyon değeri bir adrestir (i64); çağrı için gerçek fonksiyon
        // tipine köprülenir (bkz. functionAddress).
        mlir::Value callee = ctx.builder.create<mlir::UnrealizedConversionCastOp>(
            loc, mlir::TypeRange{typeManager.convertFunction(node->target->type)},
            mlir::ValueRange{calleeValue}).getResult(0);
        auto call = ctx.builder.create<mlir::func::CallIndirectOp>(loc, callee, mlir::ValueRange(args));
        callResults.append(call.getResults().begin(), call.getResults().end());
    }

    Values results;
    unsigned nextResult = 0;
    size_t nextSlot = 0;
    for (const SemLeaf& leaf : resultLeaves) {
        const bool viaSlot = node->type.isStruct() && leaf.type.isArray();
        results.push_back(viaSlot ? arraySlots[nextSlot++] : callResults[nextResult++]);
    }
    return results;
}

// Argümanlar ve sonuç C tarafının tiplerine çevrilir (bkz. externType):
// bool ve char 32 bit olarak gider, dönüşte yeniden daraltılır.
MLIRGenerator::Values MLIRGenerator::emitExternCall(FunctionDecl* decl, FunctionCallExpr* node) {
    auto loc = locOf(node);
    mlir::Type i32 = ctx.builder.getI32Type();

    llvm::SmallVector<mlir::Value, 4> args;
    for (auto& argExpr : node->arguments) {
        mlir::Value arg = emitExpr(argExpr.get());
        if (argExpr->type.isBool() || argExpr->type.isChar())
            arg = ctx.builder.create<mlir::arith::ExtUIOp>(loc, i32, arg);
        args.push_back(arg);
    }

    auto callee = ctx.theModule.lookupSymbol<mlir::func::FuncOp>(decl->externName);
    auto call = ctx.builder.create<mlir::func::CallOp>(loc, callee, mlir::ValueRange(args));
    if (call.getNumResults() == 0) return {};

    mlir::Value result = call.getResult(0);
    if (node->type.isBool()) {
        mlir::Value zero = arithManager.buildIntConstant(0, i32, loc);
        result = ctx.builder.create<mlir::arith::CmpIOp>(
            loc, mlir::arith::CmpIPredicate::ne, result, zero);
    } else if (node->type.isChar()) {
        result = ctx.builder.create<mlir::arith::TruncIOp>(loc, ctx.builder.getIntegerType(8), result);
    } else if (node->type.isString()) {
        stringResult(result); // C fonksiyonu her zaman yeni bir string döndürür
    }
    return {result};
}

void MLIRGenerator::visit(MemberExpr* node) {
    Values values = emitMember(node);
    // Alan da struct ise tek bir değeri yoktur (bkz. visit(Identifier)).
    lastValue = node->type.isComposite() ? mlir::Value{} : values.front();
}

void MLIRGenerator::emitBuiltinCall(FunctionCallExpr* node) {
    auto loc = locOf(node);

    // Matris yerleşikleri argümanlarını kendileri üretir.
    switch (node->builtin) {
        case Builtin::Zeros: case Builtin::Ones: case Builtin::Fill:
        case Builtin::Shape: case Builtin::Rows: case Builtin::Cols:
        case Builtin::Matmul: case Builtin::Transpose: case Builtin::Sum:
            emitMatrixBuiltin(node);
            return;
        case Builtin::Await:
            emitAwait(node);
            return;
        case Builtin::Scan:  // değeri emitLeaves ile alınır (struct ya da struct listesi)
            emitScan(node);
            lastValue = mlir::Value{};
            return;
        case Builtin::Print:
            if (node->arguments.size() == 1 && node->arguments[0]->type.isMatrix()) {
                emitMatrixPrint(node->arguments[0].get(), loc);
                lastValue = mlir::Value{};
                return;
            }
            break;
        default:
            break;
    }

    // Liste yerleşikleri: liste (ve eleman) struct içeriyorsa birden çok
    // değerden oluşur; argümanlarını kendileri üretir (MLIRGeneratorList.cpp).
    const bool onList = !node->arguments.empty() && node->arguments[0]->type.isList();
    switch (node->builtin) {
        case Builtin::Push:
            emitListPush(node);
            lastValue = mlir::Value{};
            return;
        case Builtin::Pop: {
            Values values = emitListPop(node);
            lastValue = node->type.isComposite() ? mlir::Value{} : values.front();
            return;
        }
        case Builtin::Free:
            if (!onList) break;
            emitListFree(node);
            lastValue = mlir::Value{};
            return;
        case Builtin::Len:
            if (!onList) break;
            lastValue = ctx.builder.create<mlir::arith::IndexCastOp>(
                loc, ctx.builder.getI64Type(),
                listManager.length(emitLeaves(node->arguments[0].get(), 0, 1).front(), loc));
            return;
        default:
            break;
    }

    llvm::SmallVector<mlir::Value, 4> args;
    llvm::SmallVector<SemType, 4> types;
    for (auto& argExpr : node->arguments) {
        args.push_back(emitExpr(argExpr.get()));
        types.push_back(argExpr->type);
    }
    lastValue = mlir::Value{};

    switch (node->builtin) {
        case Builtin::Print:
            builtinManager.buildPrint(args, types, loc);
            return;

        case Builtin::Time:
            lastValue = runtime.callValue("marlin_time", {}, ctx.builder.getF64Type(), loc);
            return;

        // free(s): stringlerin belleği kendiliğinden bırakılır; çağrı bir şey
        // yapmaz (eski kod derlenmeye devam etsin diye kabul edilir).
        case Builtin::Free:
            return;

        case Builtin::Str:
            lastValue = builtinManager.buildToString(args[0], types[0], loc);
            // (str(s) stringin kendisidir; yeni bir değer üretmez.)
            if (!types[0].isString()) stringResult(lastValue);
            return;
        case Builtin::ToInt:
        case Builtin::ToFloat: {
            mlir::Value line = arithManager.buildIntConstant(node->line, ctx.builder.getI32Type(), loc);
            lastValue = node->builtin == Builtin::ToInt
                ? runtime.callValue("marlin_str_to_i64", {args[0], line}, ctx.builder.getI64Type(), loc)
                : runtime.callValue("marlin_str_to_f64", {args[0], line}, ctx.builder.getF64Type(), loc);
            return;
        }

        // len: dizide sabit, stringde bloğun başlığındaki uzunluk (listede
        // çalışma zamanı uzunluğu; yukarıda ele alındı). Sonuç int64.
        case Builtin::Len: {
            mlir::Type i64 = ctx.builder.getI64Type();
            if (types[0].isArray())
                lastValue = arithManager.buildIntConstant(types[0].arraySize, i64, loc);
            else
                lastValue = runtime.callValue("marlin_str_len", {args[0]}, i64, loc);
            return;
        }

        default: // matematik
            lastValue = builtinManager.buildMath(node->builtin, args, types.front(), loc);
            return;
    }
}

// ===========================================================================
// İşleçler
// ===========================================================================

// Sonucu matris olan işleçler (a + b, m * 2, -m, eleman tipi dönüşümü) tek
// tek üretilmez; ifadenin tamamı tek bir işlemde birleştirilir
// (bkz. emitMatrixElementwise).

void MLIRGenerator::visit(UnaryExpr* node) {
    if (node->type.isMatrix()) return emitMatrixElementwise(node);
    mlir::Value operand = emitExpr(node->operand.get());
    lastValue = arithManager.buildUnary(node->op, operand, node->operand->type, locOf(node));
}

void MLIRGenerator::visit(BinaryExpr* node) {
    if (node->type.isMatrix()) return emitMatrixElementwise(node);
    auto loc = locOf(node);
    mlir::Value left = emitExpr(node->left.get());

    // && ve || kısa devrelidir: sağ taraf yalnızca gerekirse çalışır
    // (ör. `i < n && dizi[i] > 0`).
    if (node->op == BinaryExpr::And || node->op == BinaryExpr::Or) {
        lastValue = cfManager.buildShortCircuit(
            loc, left, node->op == BinaryExpr::And,
            [&] {
                // Sağ taraf kendi bölgesinde üretilir; orada doğan matris
                // geçicileri de orada bırakılmalı.
                const size_t mark = temporaries.size();
                mlir::Value right = emitExpr(node->right.get());
                flushTemporaries(loc, mark);
                return right;
            });
        return;
    }

    mlir::Value right = emitExpr(node->right.get());
    if (node->left->type.isString()) {
        lastValue = builtinManager.buildStringBinary(node->op, left, right, loc);
        if (node->op == BinaryExpr::Add) stringResult(lastValue);
        return;
    }
    lastValue = arithManager.buildBinary(node->op, left, right, node->left->type, loc);
}

void MLIRGenerator::visit(CastExpr* node) {
    if (node->type.isMatrix()) return emitMatrixElementwise(node);
    mlir::Value operand = emitExpr(node->operand.get());
    lastValue = arithManager.buildCast(operand, node->operand->type, node->type, locOf(node));
}

// ===========================================================================
// Lambda  (params) -> returnType | expr
//         (params) -> returnType { block }
//
// Modüle özel (private) bir func.func eklenir; ifadenin değeri o fonksiyonu
// gösteren func.constant'tır. Lambdalar dış değişken yakalayamaz (Sema
// engeller), bu yüzden kapanış (closure) verisi taşımaya gerek yoktur.
// ===========================================================================

void MLIRGenerator::visit(LambdaExpr* node) {
    auto loc = locOf(node);
    std::string lambdaName = "__lambda_" + std::to_string(lambdaCounter++);
    mlir::FunctionType funcType = typeManager.convertFunction(node->type);

    mlir::func::FuncOp lambdaOp;
    {
        mlir::OpBuilder::InsertionGuard guard(ctx.builder);
        ctx.builder.setInsertionPointToEnd(ctx.theModule.getBody());
        lambdaOp = ctx.builder.create<mlir::func::FuncOp>(loc, lambdaName, funcType);
        lambdaOp.setPrivate();
    }

    const bool savedMain = emittingMain;
    emittingMain = false; // lambda main'in içinde yazılmış olsa da main değildir
    emitFunctionBody(lambdaOp, node->type, node->params, loc, [&] {
        if (node->blockBody) {
            node->blockBody->accept(*this);
            return;
        }
        if (!node->type.returnType().isVoid()) {
            emitReturn(node->exprBody.get(), node->line, loc);
            return;
        }
        emitExpr(node->exprBody.get());
        flushTemporaries(loc);
        ctx.builder.create<mlir::func::ReturnOp>(loc, mlir::ValueRange());
    });
    emittingMain = savedMain;

    lastValue = functionAddress(lambdaName, node->type, loc);
}

// Fonksiyon değeri, fonksiyonun adresidir (i64; bkz. MLIRTypeManager). MLIR'de
// bir fonksiyonun adresi doğrudan tamsayı olarak alınamaz: önce func.constant
// ile fonksiyon tipinde bir değer üretilir, sonra bir tip köprüsüyle
// (unrealized_conversion_cast) i64'e geçilir. Çağrıda tersi yapılır. LLVM'e
// indirildikten sonra bu köprüler ptrtoint / inttoptr olur (bkz.
// LLVMLowering.cpp, lowerFunctionAddressCasts).
mlir::Value MLIRGenerator::functionAddress(const std::string& symbol, const SemType& type,
                                           mlir::Location loc) {
    mlir::Value function = ctx.builder.create<mlir::func::ConstantOp>(
        loc, typeManager.convertFunction(type), mlir::FlatSymbolRefAttr::get(&ctx.context, symbol));
    return ctx.builder.create<mlir::UnrealizedConversionCastOp>(
        loc, mlir::TypeRange{ctx.builder.getI64Type()}, mlir::ValueRange{function}).getResult(0);
}
