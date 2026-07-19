//
// Created by Eminf on 19.07.2026.
//

#ifndef MLIRCONTROLFLOWMANAGER_H
#define MLIRCONTROLFLOWMANAGER_H

#include "CodegenContext.h"
#include "mlir/Dialect/SCF/IR/SCF.h"
#include <functional>

class MLIRControlFlowManager {
    CodegenContext& ctx;
public:
    MLIRControlFlowManager(CodegenContext& context) : ctx(context) {}

    void buildIf(mlir::Value cond, mlir::Location loc,
                 std::function<void()> trueBodyGen,
                 std::function<void()> falseBodyGen) {
        mlir::OpBuilder& builder = ctx.builder;
        bool hasElse = (falseBodyGen != nullptr);
        auto ifOp = builder.create<mlir::scf::IfOp>(loc, cond, hasElse);

        builder.setInsertionPointToStart(&ifOp.getThenRegion().front());
        trueBodyGen();
        builder.create<mlir::scf::YieldOp>(loc, mlir::ValueRange());

        if (hasElse) {
            builder.setInsertionPointToStart(&ifOp.getElseRegion().front());
            falseBodyGen();
            builder.create<mlir::scf::YieldOp>(loc, mlir::ValueRange());
        }

        builder.setInsertionPointAfter(ifOp);
    }

    void buildWhile(mlir::Location loc,
                    std::function<mlir::Value()> condGen,
                    std::function<void()> bodyGen) {
        mlir::OpBuilder& builder = ctx.builder;
        // scf.while with no loop-carried values: resultTypes=[], initArgs=[]
        llvm::SmallVector<mlir::Type, 0> resultTypes;
        llvm::SmallVector<mlir::Value, 0> initArgs;
        auto whileOp = builder.create<mlir::scf::WhileOp>(
            loc,
            mlir::TypeRange(resultTypes),
            mlir::ValueRange(initArgs)
        );

        // Before block (condition region) — no block args needed
        mlir::Block* before = builder.createBlock(&whileOp.getBefore());
        builder.setInsertionPointToEnd(before);
        mlir::Value cond = condGen();
        builder.create<mlir::scf::ConditionOp>(loc, cond, mlir::ValueRange(initArgs));

        // After block (body region)
        mlir::Block* after = builder.createBlock(&whileOp.getAfter());
        builder.setInsertionPointToEnd(after);
        bodyGen();
        builder.create<mlir::scf::YieldOp>(loc, mlir::ValueRange(initArgs));

        builder.setInsertionPointAfter(whileOp);
    }
};



#endif //MLIRCONTROLFLOWMANAGER_H
