// MLIRGenerator — ifadeler

#include "MLIRGenerator.h"

#include "llvm/ADT/SmallVector.h"
#include "mlir/Dialect/Arith/IR/Arith.h"
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

// String'i modül düzeyinde bir LLVM global dizisi olarak saklar ve
// {ptr, i64} fat-pointer struct'ına dönüştürür.
void MLIRGenerator::visit(StringLiteral* node) {
    auto loc = locOf(node);
    std::string globalName = "__str_" + std::to_string(stringLiteralCount++);

    // Global sabit dizi (null sonlandırmalı)
    {
        mlir::OpBuilder::InsertionGuard guard(ctx.builder);
        ctx.builder.setInsertionPointToStart(ctx.theModule.getBody());

        auto strType = mlir::LLVM::LLVMArrayType::get(
            ctx.builder.getIntegerType(8),
            static_cast<unsigned>(node->value.size() + 1));

        ctx.builder.create<mlir::LLVM::GlobalOp>(
            loc, strType, /*isConst=*/true,
            mlir::LLVM::Linkage::Private,
            globalName,
            ctx.builder.getStringAttr(node->value + '\0'));
    }

    auto ptrType = mlir::LLVM::LLVMPointerType::get(&ctx.context);
    auto addrOp  = ctx.builder.create<mlir::LLVM::AddressOfOp>(loc, ptrType, globalName);
    auto lenOp   = ctx.builder.create<mlir::arith::ConstantIntOp>(
        loc, static_cast<int64_t>(node->value.size()), 64);

    auto undef     = ctx.builder.create<mlir::LLVM::UndefOp>(loc, typeManager.convert(node->type));
    auto insertPtr = ctx.builder.create<mlir::LLVM::InsertValueOp>(
        loc, undef, addrOp, ctx.builder.getDenseI64ArrayAttr({0}));
    lastValue      = ctx.builder.create<mlir::LLVM::InsertValueOp>(
        loc, insertPtr, lenOp, ctx.builder.getDenseI64ArrayAttr({1}));
}

// ===========================================================================
// İsimler ve diziler
// ===========================================================================

void MLIRGenerator::visit(Identifier* node) {
    auto loc = locOf(node);

    switch (node->ref) {
        case Identifier::Ref::Function:
            // Fonksiyonun değer olarak kullanımı (ör. değişkene atama, argüman)
            lastValue = ctx.builder.create<mlir::func::ConstantOp>(
                loc, typeManager.convertFunction(node->type),
                mlir::FlatSymbolRefAttr::get(&ctx.context, node->name));
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
    if (node->type.isList()) {
        llvm::SmallVector<mlir::Value, 8> elements;
        for (auto& elem : node->elements)
            elements.push_back(emitExpr(elem.get()));
        lastValue = listManager.create(
            typeManager.convert(node->type.elementType()), elements, locOf(node));
        return;
    }

    mlir::Value memref = createStorage(node->type, locOf(node));
    emitListInto(node, memref);
    lastValue = memref;
}

void MLIRGenerator::visit(ArrayAccessExpr* node) {
    auto loc = locOf(node);

    // matris[i, j, ...] — her boyut için bir indeks; bellek üzerinden, sınır denetimli
    if (node->target->type.isMatrix()) {
        mlir::Value buffer = emitMatrixBuffer(node->target.get());
        llvm::SmallVector<mlir::Value, 4> indices;
        for (auto& index : node->indices)
            indices.push_back(toIndex(emitExpr(index.get()), index->type, loc));
        lastValue = matrixManager.loadElement(buffer, indices, node->line, loc);
        return;
    }

    mlir::Value memref = emitExpr(node->target.get());
    mlir::Value index  = toIndex(emitExpr(node->indices[0].get()), node->indices[0]->type, loc);
    if (node->target->type.isList())
        lastValue = listManager.loadElement(memref, index, node->line, loc); // sınır denetimli
    else
        lastValue = ctx.builder.create<mlir::memref::LoadOp>(loc, memref, mlir::ValueRange(index));
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

    auto* ident = dynamic_cast<Identifier*>(node->target.get());
    const bool isDirect = ident && ident->ref == Identifier::Ref::Function;

    mlir::Value calleeValue;
    if (!isDirect) calleeValue = emitExpr(node->target.get());

    // Matris argümanı belleğiyle verilir; kopyalanmaz (çağrılan onu
    // değiştiremez). Argüman bir geçiciyse deyim sonunda bırakılır.
    llvm::SmallVector<mlir::Value, 4> args;
    for (auto& argExpr : node->arguments) {
        mlir::Value arg = emitExpr(argExpr.get());
        if (argExpr->type.isMatrix()) arg = matrixManager.toBuffer(arg, loc);
        args.push_back(arg);
    }

    mlir::Operation* callOp;
    if (isDirect) {
        // CPU'dan, matris alan/döndüren bir @GPU fonksiyona çağrı: veriyi
        // GPU'ya (ve sonucu geri) kopyalayan giriş fonksiyonu çağrılır.
        FunctionDecl* decl = functions.at(ident->name);
        const bool crossesToGpu = decl->isGpu && ctx.gpuEnabled && !ctx.onDevice &&
                                  decl->hasMatrixSignature();
        auto callee = ctx.theModule.lookupSymbol<mlir::func::FuncOp>(
            crossesToGpu ? hostEntryName(ident->name) : ident->name);
        callOp = ctx.builder.create<mlir::func::CallOp>(loc, callee, mlir::ValueRange(args));
    } else {
        callOp = ctx.builder.create<mlir::func::CallIndirectOp>(
            loc, calleeValue, mlir::ValueRange(args));
    }
    lastValue = callOp->getNumResults() > 0 ? callOp->getResult(0) : mlir::Value{};

    // Dönen matris yeni bir bellektir ve artık bu deyime aittir.
    if (node->type.isMatrix())
        setMatrixResult({matrixManager.toTensor(lastValue, /*writable=*/false, loc), lastValue});
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
            lastValue = runtime.callValue("minik_time", {}, ctx.builder.getF64Type(), loc);
            return;

        case Builtin::Push:
            listManager.push(args[0], args[1], loc);
            return;
        case Builtin::Pop:
            lastValue = listManager.pop(args[0], node->line, loc);
            return;
        case Builtin::Free:
            listManager.free(args[0], loc);
            return;

        // len: listede çalışma zamanı uzunluğu, dizide sabit, stringde
        // {ptr, uzunluk} çiftinin ikinci alanı. Sonuç int64.
        case Builtin::Len: {
            mlir::Type i64 = ctx.builder.getI64Type();
            if (types[0].isList())
                lastValue = ctx.builder.create<mlir::arith::IndexCastOp>(
                    loc, i64, listManager.length(args[0], loc));
            else if (types[0].isArray())
                lastValue = arithManager.buildIntConstant(types[0].arraySize, i64, loc);
            else
                lastValue = ctx.builder.create<mlir::LLVM::ExtractValueOp>(
                    loc, args[0], llvm::ArrayRef<int64_t>{1});
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

    emitFunctionBody(lambdaOp, node->type, node->params, loc, [&] {
        if (node->blockBody) {
            node->blockBody->accept(*this);
            return;
        }
        mlir::Value result = emitExpr(node->exprBody.get());
        if (node->exprBody->type.isMatrix() && !node->type.returnType().isVoid())
            result = takeMatrixOwnership(result, node->line, loc);
        flushTemporaries(loc);
        if (node->type.returnType().isVoid())
            ctx.builder.create<mlir::func::ReturnOp>(loc, mlir::ValueRange());
        else
            ctx.builder.create<mlir::func::ReturnOp>(loc, mlir::ValueRange(result));
    });

    lastValue = ctx.builder.create<mlir::func::ConstantOp>(
        loc, funcType, mlir::FlatSymbolRefAttr::get(&ctx.context, lambdaName));
}
