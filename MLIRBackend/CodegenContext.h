//
// Created by Eminf on 19.07.2026.
//

#ifndef CODEGENCONTEXT_H
#define CODEGENCONTEXT_H

#include "mlir/IR/Builders.h"
#include "mlir/IR/BuiltinOps.h"
#include "mlir/IR/Location.h"
#include "mlir/Dialect/MemRef/IR/MemRef.h"
#include <unordered_map>
#include <vector>
#include <string>

// GPU için üretilen fonksiyonların (func.func) işareti; kod üretimi koyar,
// indirme hattı okur.
inline constexpr const char* kGpuFunctionAttr = "marlin.gpu";

struct CodegenContext {
    mlir::OpBuilder& builder;
    mlir::MLIRContext& context;
    mlir::ModuleOp theModule;
    std::vector<std::unordered_map<std::string, mlir::Value>> symbolTable;

    // Üretilmekte olan fonksiyonun giriş bloğu. Bütün yığıt tahsisleri buraya
    // yapılır (bkz. createEntryAlloca).
    mlir::Block* allocaBlock = nullptr;

    // --- GPU hedefi (bkz. MLIRGenerator.h, "GPU") ---
    // @GPU fonksiyonlar gerçekten GPU için mi üretilsin? Kapalıysa (--gpu=off)
    // sıradan CPU fonksiyonu gibi üretilirler.
    bool gpuEnabled = true;
    // Üretilmekte olan fonksiyon @GPU: matrisleri GPU belleğinde yaşar.
    bool onDevice = false;
    // Üretilmekte olan kod GPU'da çalışacak (bir `parallel for` gövdesi):
    // çalışma zamanı kütüphanesi çağrılamaz.
    bool inKernel = false;

    CodegenContext(mlir::OpBuilder& b, mlir::MLIRContext& c, mlir::ModuleOp m)
        : builder(b), context(c), theModule(m) {
        // Push global scope
        symbolTable.push_back(std::unordered_map<std::string, mlir::Value>());
    }

    mlir::Location getLoc(llvm::StringRef file, int line, int col) {
        return mlir::FileLineColLoc::get(&context, file, line, col);
    }

    // Tahsisi, o an neresi üretiliyor olursa olsun fonksiyonun giriş bloğuna
    // koyar. Döngü gövdesinde üretilen bir alloca her yinelemede yığıtı
    // büyütürdü; ayrıca tek bloklu scf bölgelerinin içinden de her zaman
    // erişilebilir (giriş bloğu her şeye hâkimdir).
    //
    // Coroutine fonksiyonda (async.func; bkz. MLIRGeneratorTask.cpp) yer
    // yığıtta değil yığın belleğinde ayrılır (`heapLocals`): async.func bir
    // otomatik bellek kapsamı değildir, ayrıca değişkenleri askıya alma
    // noktaları boyunca yaşamalıdır. Bu yerler fonksiyon dönerken bırakılır
    // (bkz. MLIRGenerator::emitReturnOp).
    mlir::Value createEntryAlloca(mlir::Location loc, mlir::MemRefType type) {
        mlir::OpBuilder::InsertionGuard guard(builder);
        builder.setInsertionPointToStart(allocaBlock);
        if (heapLocals) {
            mlir::Value local = builder.create<mlir::memref::AllocOp>(loc, type);
            heapLocalValues.push_back(local);
            return local;
        }
        return builder.create<mlir::memref::AllocaOp>(loc, type);
    }
    bool heapLocals = false;
    std::vector<mlir::Value> heapLocalValues;
};

#endif //CODEGENCONTEXT_H
