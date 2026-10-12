#ifndef MLIRRUNTIME_H
#define MLIRRUNTIME_H

#include "CodegenContext.h"
#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/STLFunctionalExtras.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringRef.h"
#include "mlir/Dialect/Func/IR/FuncOps.h"
#include "mlir/Dialect/SCF/IR/SCF.h"

// Çalışma zamanı kütüphanesine (runtime/minik_rt.c) çağrı üretir.
//
// Fonksiyon modülde ilk kullanıldığında gövdesiz olarak bildirilir; tanımını
// bağlayıcı libminikrt.a'dan çözer. Derleyicinin C tarafına açılan tek kapısı
// burasıdır: imzalar minik_rt.c'dekilerle aynı olmalıdır.
class MLIRRuntime {
    CodegenContext& ctx;

public:
    explicit MLIRRuntime(CodegenContext& context) : ctx(context) {}

    void call(llvm::StringRef name, llvm::ArrayRef<mlir::Value> args, mlir::Location loc) {
        mlir::OpBuilder& b = ctx.builder;

        auto callee = ctx.theModule.lookupSymbol<mlir::func::FuncOp>(name);
        if (!callee) {
            llvm::SmallVector<mlir::Type, 4> inputs;
            for (mlir::Value arg : args) inputs.push_back(arg.getType());

            mlir::OpBuilder::InsertionGuard guard(b);
            b.setInsertionPointToStart(ctx.theModule.getBody());
            callee = b.create<mlir::func::FuncOp>(loc, name, b.getFunctionType(inputs, {}));
            callee.setPrivate();
        }
        b.create<mlir::func::CallOp>(loc, callee, mlir::ValueRange(args));
    }

    /// Değer döndüren çalışma zamanı fonksiyonuna çağrı.
    mlir::Value callValue(llvm::StringRef name, llvm::ArrayRef<mlir::Value> args,
                          mlir::Type resultType, mlir::Location loc) {
        mlir::OpBuilder& b = ctx.builder;

        auto callee = ctx.theModule.lookupSymbol<mlir::func::FuncOp>(name);
        if (!callee) {
            llvm::SmallVector<mlir::Type, 4> inputs;
            for (mlir::Value arg : args) inputs.push_back(arg.getType());

            mlir::OpBuilder::InsertionGuard guard(b);
            b.setInsertionPointToStart(ctx.theModule.getBody());
            callee = b.create<mlir::func::FuncOp>(loc, name, b.getFunctionType(inputs, {resultType}));
            callee.setPrivate();
        }
        return b.create<mlir::func::CallOp>(loc, callee, mlir::ValueRange(args)).getResult(0);
    }

    /// scf.if %cond { body }  — çalışma zamanı denetimleri için: koşul
    /// doğruysa gövdedeki hata çağrıları çalışır ve program durur.
    void ifThen(mlir::Value cond, mlir::Location loc, llvm::function_ref<void()> body) {
        mlir::OpBuilder& b = ctx.builder;
        auto ifOp = b.create<mlir::scf::IfOp>(loc, mlir::TypeRange(), cond,
                                              /*addThenBlock=*/true, /*addElseBlock=*/false);
        mlir::OpBuilder::InsertionGuard guard(b);
        b.setInsertionPointToEnd(&ifOp.getThenRegion().front());
        body();
        b.create<mlir::scf::YieldOp>(loc);
    }

    /// scf.if %cond { call @name(args) }
    void callIf(mlir::Value cond, llvm::StringRef name, llvm::ArrayRef<mlir::Value> args,
                mlir::Location loc) {
        ifThen(cond, loc, [&] { call(name, args, loc); });
    }
};

#endif //MLIRRUNTIME_H
