//
// Created by Eminf on 19.07.2026.
//

#include "MLIRGenerator.h"

#include "mlir/Dialect/Arith/IR/Arith.h"
#include "mlir/Dialect/LLVMIR/LLVMDialect.h"
#include "mlir/Dialect/MemRef/IR/MemRef.h"
#include "mlir/IR/BuiltinAttributes.h"
#include "mlir/IR/BuiltinTypes.h"
#include <cassert>

// ===========================================================================
// Program
// ===========================================================================

void MLIRGenerator::visit(Program* node) {
    ctx.builder.setInsertionPointToEnd(ctx.theModule.getBody());

    // 1. geçiş — globaller ve tüm fonksiyon imzaları, gövdelerden ÖNCE modüle
    // eklenir. Böylece bir fonksiyon, kaynakta kendisinden sonra tanımlanan
    // bir fonksiyonu ya da globali kullanabilir (ileri referans, karşılıklı
    // özyineleme).
    for (auto& decl : node->declarations) {
        if (auto* var = dynamic_cast<VariableDecl*>(decl.get()))
            emitGlobalVariable(var);
        else if (auto* fn = dynamic_cast<FunctionDecl*>(decl.get()))
            declareFunctionSignature(fn);
    }

    // 2. geçiş — fonksiyon gövdeleri.
    for (auto& decl : node->declarations) {
        if (dynamic_cast<FunctionDecl*>(decl.get()))
            decl->accept(*this);
    }
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
    ctx.builder.create<mlir::func::FuncOp>(
        locOf(node), node->name, typeManager.convertFunction(node->fnType));
}

void MLIRGenerator::visit(FunctionDecl* node) {
    // FuncOp, Program ziyaretinin 1. geçişinde oluşturuldu; burada gövdesi doldurulur.
    auto funcOp = ctx.theModule.lookupSymbol<mlir::func::FuncOp>(node->name);
    assert(funcOp && "fonksiyon imzasi 1. geciste olusturulmus olmali");

    emitFunctionBody(funcOp, node->fnType, node->params, locOf(node),
                     [&] { node->body->accept(*this); });
}

void MLIRGenerator::emitFunctionBody(mlir::func::FuncOp funcOp, const SemType& fnType,
                                     const std::vector<std::unique_ptr<ParamDecl>>& params,
                                     mlir::Location loc, llvm::function_ref<void()> emitBody) {
    mlir::OpBuilder::InsertionGuard guard(ctx.builder);
    mlir::Block* entryBlock = funcOp.addEntryBlock();
    ctx.builder.setInsertionPointToEnd(entryBlock);

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
    auto savedMatrixScopes = std::move(matrixScopes);
    auto savedLoopDepths = std::move(loopScopeDepths);
    temporaries.clear();
    ownedBuffers.clear();
    matrixScopes.clear();
    loopScopeDepths.clear();

    // Diziler, fonksiyon değerleri ve matrisler doğrudan bağlanır (matris
    // parametresi salt okunurdur: çağıranın belleğidir). Diğer parametreler
    // atanabilir olmaları için bir yuvaya kopyalanır.
    for (size_t i = 0; i < params.size(); ++i) {
        mlir::Value arg = entryBlock->getArgument(static_cast<unsigned>(i));
        const SemType& paramType = fnType.paramType(i);
        if (paramType.isArray() || paramType.isFunction() || paramType.isMatrix()) {
            scopeManager.declareVariable(params[i]->name, arg);
            continue;
        }
        mlir::Value storage = createStorage(paramType, loc);
        storeTo(arg, storage, paramType, loc);
        scopeManager.declareVariable(params[i]->name, storage);
    }

    emitBody();

    if (!MLIRControlFlowManager::isTerminated(ctx.builder.getInsertionBlock())) {
        if (fnType.returnType().isVoid())
            ctx.builder.create<mlir::func::ReturnOp>(loc, mlir::ValueRange());
        else
            // Sema her yolun return ettiğini doğruladı; buraya gelinemez.
            ctx.builder.create<mlir::LLVM::UnreachableOp>(loc);
    }

    temporaries = std::move(savedTemporaries);
    ownedBuffers = std::move(savedOwned);
    matrixScopes = std::move(savedMatrixScopes);
    loopScopeDepths = std::move(savedLoopDepths);
    cfManager.leaveFunction(std::move(savedFlow));
    ctx.allocaBlock = savedAllocaBlock;
    ctx.symbolTable = std::move(savedSymbols);
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

mlir::Value MLIRGenerator::emitGlobalRef(const std::string& name, mlir::Location loc) {
    auto globalOp = ctx.theModule.lookupSymbol<mlir::memref::GlobalOp>(name);
    assert(globalOp && "global 1. geciste olusturulmus olmali");
    return ctx.builder.create<mlir::memref::GetGlobalOp>(loc, globalOp.getType(), name);
}

// ===========================================================================
// Bellek yardımcıları
// ===========================================================================

mlir::Value MLIRGenerator::createStorage(const SemType& type, mlir::Location loc) {
    // string bir LLVM struct'ıdır; memref elemanı olamadığı için llvm.alloca kullanılır.
    if (type.isString()) {
        mlir::OpBuilder::InsertionGuard guard(ctx.builder);
        ctx.builder.setInsertionPointToStart(ctx.allocaBlock);
        mlir::Value one = ctx.builder.create<mlir::arith::ConstantIntOp>(loc, 1, 64);
        return ctx.builder.create<mlir::LLVM::AllocaOp>(
            loc, mlir::LLVM::LLVMPointerType::get(&ctx.context), typeManager.convert(type), one);
    }

    mlir::MemRefType memRefType = type.isArray()
        ? mlir::cast<mlir::MemRefType>(typeManager.convert(type))
        : mlir::MemRefType::get({}, typeManager.convert(type));
    return ctx.createEntryAlloca(loc, memRefType);
}

void MLIRGenerator::storeTo(mlir::Value value, mlir::Value storage, const SemType& type,
                            mlir::Location loc) {
    if (type.isArray())
        ctx.builder.create<mlir::memref::CopyOp>(loc, value, storage);
    else if (type.isString())
        ctx.builder.create<mlir::LLVM::StoreOp>(loc, value, storage);
    else
        ctx.builder.create<mlir::memref::StoreOp>(loc, value, storage);
}

mlir::Value MLIRGenerator::loadFrom(mlir::Value storage, const SemType& type, mlir::Location loc) {
    // Diziler ve fonksiyon değerleri zaten kendileridir.
    if (type.isArray() || type.isFunction())
        return storage;
    if (type.isString())
        return ctx.builder.create<mlir::LLVM::LoadOp>(loc, typeManager.convert(type), storage);
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
    if (type.isString())
        return ctx.builder.create<mlir::LLVM::ZeroOp>(loc, mlirType);
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
        mlir::Value index = ctx.builder.create<mlir::arith::ConstantIndexOp>(
            loc, static_cast<int64_t>(i));
        ctx.builder.create<mlir::memref::StoreOp>(loc, elem, memref, mlir::ValueRange(index));
    }
}
