//
// Created by Eminf on 19.07.2026.
//

#include "MLIRGenerator.h"

#include "mlir/Dialect/Arith/IR/Arith.h"
#include "mlir/Dialect/Async/IR/Async.h"
#include "mlir/Dialect/LLVMIR/LLVMDialect.h"
#include "mlir/Dialect/MemRef/IR/MemRef.h"
#include "mlir/IR/BuiltinAttributes.h"
#include "mlir/IR/BuiltinTypes.h"
#include <cassert>

// ===========================================================================
// Program
// ===========================================================================

void MLIRGenerator::visit(Program* node) {
    program = node;
    ctx.builder.setInsertionPointToEnd(ctx.theModule.getBody());

    // Bütün modüllerin bildirimleri tek bir MLIR modülüne üretilir; isimler
    // modül önekiyle ayrıştığı için (bkz. Module::prefix) çakışmazlar.
    //
    // 1. geçiş — globaller ve tüm fonksiyon imzaları, gövdelerden ÖNCE modüle
    // eklenir. Böylece bir fonksiyon, kaynakta kendisinden sonra tanımlanan
    // bir fonksiyonu ya da globali kullanabilir (ileri referans, karşılıklı
    // özyineleme).
    for (auto& decl : node->declarations) {
        if (auto* var = dynamic_cast<VariableDecl*>(decl.get()))
            emitGlobalVariable(var);
        else if (auto* fn = dynamic_cast<FunctionDecl*>(decl.get())) {
            functions[fn->name] = fn;
            declareFunctionSignature(fn);
        }
    }

    // 2. geçiş — fonksiyon gövdeleri.
    for (auto& decl : node->declarations) {
        if (auto* fn = dynamic_cast<FunctionDecl*>(decl.get()); fn && !fn->isExtern)
            decl->accept(*this);
    }
}

// extern fonksiyonların C tarafındaki tipi. Çağrı kuralında 32 bitten dar
// tamsayıların üst bitlerini kimin dolduracağı platforma bağlı olduğundan
// bool ve char 32 bit olarak geçirilir (runtime'daki imzalar da böyledir).
mlir::Type MLIRGenerator::externType(const SemType& type) {
    if (type.isBool() || type.isChar()) return ctx.builder.getI32Type();
    return typeManager.convert(type);
}

// Tip ve parametre düğümleri ziyaretçi arayüzü gereği tanımlı; üretim gerektirmez.
void MLIRGenerator::visit(Type* /*node*/) {}
void MLIRGenerator::visit(ParamDecl* /*node*/) {}

// ===========================================================================
// Fonksiyonlar
// ===========================================================================

void MLIRGenerator::declareFunctionSignature(FunctionDecl* node) {
    mlir::OpBuilder::InsertionGuard guard(ctx.builder);
    ctx.builder.setInsertionPointToEnd(ctx.theModule.getBody());

    // extern: gövdesiz bildirim, C'deki adıyla. Tanımını bağlayıcı çalışma
    // zamanı kütüphanesinden çözer.
    if (node->isExtern) {
        llvm::SmallVector<mlir::Type, 4> inputs, results;
        for (size_t i = 0; i < node->fnType.paramCount(); ++i)
            inputs.push_back(externType(node->fnType.paramType(i)));
        if (!node->fnType.returnType().isVoid())
            results.push_back(externType(node->fnType.returnType()));
        auto declaration = ctx.builder.create<mlir::func::FuncOp>(
            locOf(node), node->externName, ctx.builder.getFunctionType(inputs, results));
        declaration.setPrivate();
        return;
    }

    // Görevden ulaşılan ve bekleyebilen fonksiyon coroutine olur (bkz.
    // MLIRGeneratorTask.cpp).
    if (node->isAsync) {
        usesTasks = true;
        ctx.builder.create<mlir::async::FuncOp>(locOf(node), node->name,
                                                typeManager.convertAsyncFunction(node->fnType));
        return;
    }

    auto funcType = typeManager.convertFunction(node->fnType);
    auto funcOp = ctx.builder.create<mlir::func::FuncOp>(locOf(node), node->name, funcType);
    if (!targetsGpu(node)) return;

    // İndirme hattı, paralel döngüleri GPU'ya çevrilecek fonksiyonları bu
    // işaretten tanır.
    usesGpu = true;
    funcOp->setAttr(kGpuFunctionAttr, ctx.builder.getUnitAttr());
    if (node->hasMatrixSignature())
        ctx.builder.create<mlir::func::FuncOp>(locOf(node), hostEntryName(node->name), funcType);
}

void MLIRGenerator::visit(FunctionDecl* node) {
    // Fonksiyon, Program ziyaretinin 1. geçişinde oluşturuldu; burada gövdesi
    // doldurulur. (func.func ya da coroutine ise async.func)
    auto funcOp = mlir::dyn_cast_or_null<mlir::FunctionOpInterface>(
        ctx.theModule.lookupSymbol(node->name));
    assert(funcOp && "fonksiyon imzasi 1. geciste olusturulmus olmali");

    ctx.onDevice = targetsGpu(node);
    emittingMain = node->name == "main";
    emitFunctionBody(funcOp, node->fnType, node->params, locOf(node),
                     [&] { node->body->accept(*this); });
    emittingMain = false;
    ctx.onDevice = false;

    if (targetsGpu(node) && node->hasMatrixSignature())
        emitHostEntry(node);
}

// @GPU fonksiyonun CPU'dan çağrılan girişi. CPU ile GPU arasındaki bütün veri
// taşıma buradadır:
//
//   func @f.host(%a: memref (CPU), %n) -> memref (CPU) {
//     %da = gpu.alloc ...;  gpu.memcpy %da, %a      matris parametreleri GPU'ya
//     %dr = call @f(%da, %n)                        iş GPU'da yapılır
//     gpu.dealloc %da
//     %r = alloc ...;  gpu.memcpy %r, %dr           sonuç CPU'ya
//     gpu.dealloc %dr
//     return %r
//   }
//
// Matris parametreleri salt okunur olduğu için geri kopyalanmaz. Sayılar
// olduğu gibi geçer.
void MLIRGenerator::emitHostEntry(FunctionDecl* node) {
    auto loc = locOf(node);
    auto target = ctx.theModule.lookupSymbol<mlir::func::FuncOp>(node->name);
    auto entry = ctx.theModule.lookupSymbol<mlir::func::FuncOp>(hostEntryName(node->name));

    mlir::OpBuilder::InsertionGuard guard(ctx.builder);
    mlir::Block* block = entry.addEntryBlock();
    ctx.builder.setInsertionPointToEnd(block);

    llvm::SmallVector<mlir::Value, 4> args, uploaded;
    for (size_t i = 0; i < node->params.size(); ++i) {
        mlir::Value arg = block->getArgument(static_cast<unsigned>(i));
        if (node->fnType.paramType(i).isMatrix()) {
            arg = matrixManager.upload(arg, loc);
            uploaded.push_back(arg);
        }
        args.push_back(arg);
    }

    auto call = ctx.builder.create<mlir::func::CallOp>(loc, target, mlir::ValueRange(args));
    for (mlir::Value buffer : uploaded)
        matrixManager.free(buffer, /*onDevice=*/true, loc);

    if (call.getNumResults() == 0) {
        ctx.builder.create<mlir::func::ReturnOp>(loc, mlir::ValueRange());
        return;
    }
    mlir::Value result = call.getResult(0);
    if (node->fnType.returnType().isMatrix()) {
        mlir::Value deviceResult = result;
        result = matrixManager.download(deviceResult, loc);
        matrixManager.free(deviceResult, /*onDevice=*/true, loc);
    }
    ctx.builder.create<mlir::func::ReturnOp>(loc, mlir::ValueRange(result));
}

void MLIRGenerator::emitFunctionBody(mlir::FunctionOpInterface funcOp, const SemType& fnType,
                                     const std::vector<std::unique_ptr<ParamDecl>>& params,
                                     mlir::Location loc, llvm::function_ref<void()> emitBody) {
    mlir::OpBuilder::InsertionGuard guard(ctx.builder);
    mlir::Block* entryBlock = funcOp.addEntryBlock();
    ctx.builder.setInsertionPointToEnd(entryBlock);
    const bool savedAsync = emittingAsync;
    emittingAsync = mlir::isa<mlir::async::FuncOp>(funcOp.getOperation());
    const bool savedHeapLocals = ctx.heapLocals;
    auto savedHeapLocalValues = std::move(ctx.heapLocalValues);
    ctx.heapLocals = emittingAsync;
    ctx.heapLocalValues.clear();

    // Fonksiyona özgü durum: iç içe üretilen bir lambda, dıştaki fonksiyonun
    // değişkenlerini, döngülerini ya da giriş bloğunu görmemeli.
    auto savedSymbols = std::move(ctx.symbolTable);
    ctx.symbolTable.clear();
    ctx.symbolTable.emplace_back();
    mlir::Block* savedAllocaBlock = ctx.allocaBlock;
    ctx.allocaBlock = entryBlock;
    auto savedFlow = cfManager.enterFunction();
    // Matris sahipliği de fonksiyona özgüdür (geçiciler, blok değişkenleri).
    auto savedTemporaries = std::move(temporaries);
    auto savedOwned = std::move(ownedBuffers);
    auto savedMatrixScopes = std::move(ownedScopes);
    auto savedLoopDepths = std::move(loopScopeDepths);
    temporaries.clear();
    ownedBuffers.clear();
    ownedScopes.clear();
    ownedScopes.emplace_back(); // fonksiyonun kendisi: string parametreler
    loopScopeDepths.clear();

    auto savedReturnSlots = std::move(returnArraySlots);
    returnArraySlots.clear();

    // Diziler ve matrisler doğrudan bağlanır (matris
    // parametresi salt okunurdur: çağıranın belleğidir). Diğer parametreler
    // atanabilir olmaları için bir yuvaya kopyalanır. Struct parametre yaprak
    // yaprak gelir; her yaprak "p.alan" adıyla ayrı bir değişken olur.
    // String parametre fonksiyonun kendi başvurusunu tutar (çıkışta bırakılır).
    unsigned argIndex = 0;
    for (size_t i = 0; i < params.size(); ++i) {
        for (const SemLeaf& leaf : leavesOf(fnType.paramType(i), params[i]->name)) {
            mlir::Value arg = entryBlock->getArgument(argIndex++);
            if (leaf.type.isArray() || leaf.type.isMatrix()) {
                scopeManager.declareVariable(leaf.path, arg);
                continue;
            }
            mlir::Value storage = createStorage(leaf.type, loc);
            storeTo(arg, storage, leaf.type, loc);
            if (isHandle(leaf.type)) {
                retainHandle(arg, leaf.type, loc);
                ownedScopes.back().push_back({storage, leaf.type});
            }
            scopeManager.declareVariable(leaf.path, storage);
        }
    }
    // Kalan argümanlar: dönen struct'ın dizi alanlarının yazılacağı yerler.
    for (; argIndex < entryBlock->getNumArguments(); ++argIndex)
        returnArraySlots.push_back(entryBlock->getArgument(argIndex));

    emitBody();

    if (!MLIRControlFlowManager::isTerminated(ctx.builder.getInsertionBlock())) {
        if (fnType.returnType().isVoid()) {
            freeScopes(0, loc);
            emitReturnOp({}, loc);
        } else
            // Sema her yolun return ettiğini doğruladı; buraya gelinemez.
            ctx.builder.create<mlir::LLVM::UnreachableOp>(loc);
    }

    returnArraySlots = std::move(savedReturnSlots);
    temporaries = std::move(savedTemporaries);
    ownedBuffers = std::move(savedOwned);
    ownedScopes = std::move(savedMatrixScopes);
    loopScopeDepths = std::move(savedLoopDepths);
    cfManager.leaveFunction(std::move(savedFlow));
    ctx.allocaBlock = savedAllocaBlock;
    ctx.symbolTable = std::move(savedSymbols);
    emittingAsync = savedAsync;
    ctx.heapLocals = savedHeapLocals;
    ctx.heapLocalValues = std::move(savedHeapLocalValues);
}

// ===========================================================================
// Globaller
// ===========================================================================

namespace {
// Sabit bir başlangıç ifadesinin (literal, eksili literal, dönüşüm) değeri.
struct ConstValue {
    bool isFloat = false;
    double f = 0;
    int64_t i = 0;
};

ConstValue evalConstant(Expression* expr) {
    if (auto* lit = dynamic_cast<IntegerLiteral*>(expr)) return {false, 0, lit->value};
    if (auto* lit = dynamic_cast<FloatLiteral*>(expr))   return {true, lit->value, 0};
    if (auto* lit = dynamic_cast<BooleanLiteral*>(expr)) return {false, 0, lit->value ? 1 : 0};
    if (auto* lit = dynamic_cast<CharLiteral*>(expr))
        return {false, 0, static_cast<unsigned char>(lit->value)};
    if (auto* cast = dynamic_cast<CastExpr*>(expr)) return evalConstant(cast->operand.get());
    if (auto* unary = dynamic_cast<UnaryExpr*>(expr)) {
        ConstValue v = evalConstant(unary->operand.get());
        v.f = -v.f;
        v.i = -v.i;
        return v;
    }
    return {};
}
} // namespace

mlir::Attribute MLIRGenerator::constantAttr(Expression* expr, const SemType& type) {
    ConstValue value = evalConstant(expr);
    mlir::Type mlirType = typeManager.convert(type);

    if (auto floatType = mlir::dyn_cast<mlir::FloatType>(mlirType)) {
        bool lossy = false;
        llvm::APFloat apValue(value.isFloat ? value.f : static_cast<double>(value.i));
        apValue.convert(floatType.getFloatSemantics(), llvm::APFloat::rmNearestTiesToEven, &lossy);
        return ctx.builder.getFloatAttr(floatType, apValue);
    }

    int64_t intValue = value.isFloat ? static_cast<int64_t>(value.f) : value.i;
    llvm::APInt bits = llvm::APInt(64, static_cast<uint64_t>(intValue))
                           .zextOrTrunc(mlirType.getIntOrFloatBitWidth());
    return ctx.builder.getIntegerAttr(mlirType, bits);
}

// Global değişken → memref.global. Başlangıç değeri yoksa sıfırla başlatılır
// (initial_value'suz bir memref.global dış bildirim sayılırdı).
void MLIRGenerator::emitGlobalVariable(VariableDecl* node) {
    const SemType& type = node->varType;
    // Struct: her yaprağı ayrı bir global ("havuz.hata"); hepsi sıfırla başlar.
    if (type.isStruct()) {
        for (const SemLeaf& leaf : leavesOf(type, node->name))
            emitZeroGlobal(leaf.path, leaf.type, locOf(node));
        return;
    }
    // String: boş string (0 adresi) ile başlar.
    if (type.isString()) {
        emitZeroGlobal(node->name, type, locOf(node));
        return;
    }
    mlir::MemRefType memRefType = type.isArray()
        ? mlir::cast<mlir::MemRefType>(typeManager.convert(type))
        : mlir::MemRefType::get({}, typeManager.convert(type));

    // memref.global, başlangıç değerini memref ile aynı şekle sahip bir
    // elements attribute olarak ister.
    auto tensorType = mlir::RankedTensorType::get(
        memRefType.getShape(), memRefType.getElementType());
    mlir::Attribute initAttr = node->initializer
        ? mlir::Attribute(mlir::DenseElementsAttr::get(
              tensorType, constantAttr(node->initializer.get(), type)))
        : ctx.builder.getZeroAttr(tensorType);

    mlir::OpBuilder::InsertionGuard guard(ctx.builder);
    ctx.builder.setInsertionPointToEnd(ctx.theModule.getBody());
    ctx.builder.create<mlir::memref::GlobalOp>(
        locOf(node),
        node->name,
        ctx.builder.getStringAttr("private"),
        memRefType,
        initAttr,
        /*constant=*/false,
        /*alignment=*/nullptr);
}

void MLIRGenerator::emitZeroGlobal(const std::string& name, const SemType& type,
                                   mlir::Location loc) {
    auto memRefType = mlir::MemRefType::get({}, typeManager.convert(type));
    auto tensorType = mlir::RankedTensorType::get({}, memRefType.getElementType());
    mlir::OpBuilder::InsertionGuard guard(ctx.builder);
    ctx.builder.setInsertionPointToEnd(ctx.theModule.getBody());
    ctx.builder.create<mlir::memref::GlobalOp>(loc, name, ctx.builder.getStringAttr("private"),
                                               memRefType, ctx.builder.getZeroAttr(tensorType),
                                               /*constant=*/false, /*alignment=*/nullptr);
    if (type.isString()) globalStrings.push_back(name);
}

mlir::Value MLIRGenerator::emitGlobalRef(const std::string& name, mlir::Location loc) {
    auto globalOp = ctx.theModule.lookupSymbol<mlir::memref::GlobalOp>(name);
    assert(globalOp && "global 1. geciste olusturulmus olmali");
    return ctx.builder.create<mlir::memref::GetGlobalOp>(loc, globalOp.getType(), name);
}

// ===========================================================================
// Bellek yardımcıları
// ===========================================================================

mlir::Value MLIRGenerator::createStorage(const SemType& type, mlir::Location loc) {
    mlir::MemRefType memRefType = type.isArray()
        ? mlir::cast<mlir::MemRefType>(typeManager.convert(type))
        : mlir::MemRefType::get({}, typeManager.convert(type));
    return ctx.createEntryAlloca(loc, memRefType);
}

void MLIRGenerator::storeTo(mlir::Value value, mlir::Value storage, const SemType& type,
                            mlir::Location loc) {
    if (type.isArray())
        ctx.builder.create<mlir::memref::CopyOp>(loc, value, storage);
    else
        ctx.builder.create<mlir::memref::StoreOp>(loc, value, storage);
}

mlir::Value MLIRGenerator::loadFrom(mlir::Value storage, const SemType& type, mlir::Location loc) {
    // Diziler zaten kendileridir.
    if (type.isArray())
        return storage;
    // Matris parametresi doğrudan belleğe bağlıdır; yerel matris değişkeni
    // ise belleği tutan bir yuvadır (aşağıda okunur).
    if (type.isMatrix() && storage.getType() == typeManager.convert(type))
        return storage;
    // Skaler: değişkenler memref<T> yuvasındadır; döngü değişkenleri ise
    // doğrudan SSA değeridir.
    if (mlir::isa<mlir::MemRefType>(storage.getType()))
        return ctx.builder.create<mlir::memref::LoadOp>(loc, storage);
    return storage;
}

mlir::Value MLIRGenerator::emitZero(const SemType& type, mlir::Location loc) {
    mlir::Type mlirType = typeManager.convert(type);
    // (String için sıfır adresi boş string demektir.)
    if (auto floatType = mlir::dyn_cast<mlir::FloatType>(mlirType))
        return arithManager.buildFloatConstant(0.0, floatType, loc);
    return arithManager.buildIntConstant(0, mlirType, loc);
}

mlir::Value MLIRGenerator::toIndex(mlir::Value value, const SemType& type, mlir::Location loc) {
    mlir::Type indexType = ctx.builder.getIndexType();
    if (value.getType() == indexType) return value;
    if (type.isSigned)
        return ctx.builder.create<mlir::arith::IndexCastOp>(loc, indexType, value);
    return ctx.builder.create<mlir::arith::IndexCastUIOp>(loc, indexType, value);
}

void MLIRGenerator::emitListInto(ListLiteral* list, mlir::Value memref) {
    auto loc = locOf(list);
    for (size_t i = 0; i < list->elements.size(); ++i) {
        mlir::Value elem = emitExpr(list->elements[i].get());
        if (list->elements[i]->type.isString()) elem = ownString(elem, loc); // eleman diziye aittir
        mlir::Value index = ctx.builder.create<mlir::arith::ConstantIndexOp>(
            loc, static_cast<int64_t>(i));
        ctx.builder.create<mlir::memref::StoreOp>(loc, elem, memref, mlir::ValueRange(index));
    }
}
