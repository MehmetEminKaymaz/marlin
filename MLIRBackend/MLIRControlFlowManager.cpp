//
// Created by Eminf on 19.07.2026.
//

#include "MLIRControlFlowManager.h"

#include "mlir/Dialect/Arith/IR/Arith.h"
#include "mlir/Dialect/ControlFlow/IR/ControlFlowOps.h"
#include "mlir/Dialect/LLVMIR/LLVMDialect.h"
#include "mlir/Dialect/SCF/IR/SCF.h"
#include <cassert>

using namespace mlir;

bool MLIRControlFlowManager::isTerminated(Block* block) {
    return block && !block->empty() && block->back().hasTrait<OpTrait::IsTerminator>();
}

MLIRControlFlowManager::State MLIRControlFlowManager::enterFunction() {
    State saved{std::move(loopStack), cfgAllowed};
    loopStack.clear();
    cfgAllowed = true;
    return saved;
}

void MLIRControlFlowManager::leaveFunction(State saved) {
    loopStack = std::move(saved.loopStack);
    cfgAllowed = saved.cfgAllowed;
}

// ===========================================================================
// break / continue
// ===========================================================================

void MLIRControlFlowManager::emitBreak(Location loc) {
    assert(!loopStack.empty() && loopStack.back().exitBlock &&
           "break: Sema bu donguyu bloklu (needsCFG) isaretlemis olmali");
    ctx.builder.create<cf::BranchOp>(loc, loopStack.back().exitBlock);
}

void MLIRControlFlowManager::emitContinue(Location loc) {
    assert(!loopStack.empty() && "continue: aktif dongu yok");
    ctx.builder.create<cf::BranchOp>(loc, loopStack.back().continueBlock);
}

// ===========================================================================
// Yardımcılar
// ===========================================================================

// Yapısal bağlamdaysak (scf bölgesi içinde) bloklu yapıyı bir
// scf.execute_region içine sarar; değilsek doğrudan üretir.
//
//   scf.execute_region {
//     ^entry:  ... bloklu yapı ...
//     ^exit:   scf.yield
//   }
void MLIRControlFlowManager::withCFG(Location loc, llvm::function_ref<void()> emit) {
    if (cfgAllowed) {
        emit();
        return;
    }

    OpBuilder& builder = ctx.builder;
    auto regionOp = builder.create<scf::ExecuteRegionOp>(loc, TypeRange());

    OpBuilder::InsertionGuard guard(builder);
    builder.createBlock(&regionOp.getRegion());
    cfgAllowed = true;
    emit();
    // Sarılan yapı bir döngüdür ve çıkış bloğunda biter.
    builder.create<scf::YieldOp>(loc);
    cfgAllowed = false;
}

// Yapısal döngü gövdesi. `continue` yoksa gövde doğrudan döngünün tek
// bloğuna üretilir. Varsa:
//
//   scf.for ... {
//     scf.execute_region {
//       ^entry:  ... gövde; continue → br ^latch ...
//                br ^latch
//       ^latch:  scf.yield
//     }
//   }
void MLIRControlFlowManager::emitStructuredLoopBody(Location loc, bool hasContinue,
                                                    llvm::function_ref<void()> bodyGen) {
    OpBuilder& builder = ctx.builder;
    const bool savedCfg = cfgAllowed;

    if (!hasContinue) {
        cfgAllowed = false;
        bodyGen();
        cfgAllowed = savedCfg;
        return;
    }

    auto regionOp = builder.create<scf::ExecuteRegionOp>(loc, TypeRange());
    {
        OpBuilder::InsertionGuard guard(builder);
        Block* entry = builder.createBlock(&regionOp.getRegion());
        Block* latch = builder.createBlock(&regionOp.getRegion());
        builder.create<scf::YieldOp>(loc);

        builder.setInsertionPointToEnd(entry);
        cfgAllowed = true;
        loopStack.push_back({latch, nullptr});
        bodyGen();
        if (!isTerminated(builder.getInsertionBlock()))
            builder.create<cf::BranchOp>(loc, latch);
        loopStack.pop_back();
    }
    cfgAllowed = savedCfg;
}

// ===========================================================================
// if / else
// ===========================================================================

void MLIRControlFlowManager::buildIf(Location loc, Value cond, bool structured,
                                     BodyGen thenGen, BodyGen elseGen) {
    if (!structured) {
        buildIfCFG(loc, cond, thenGen, elseGen);
        return;
    }

    //   scf.if %cond { ... } else { ... }
    OpBuilder& builder = ctx.builder;
    auto ifOp = builder.create<scf::IfOp>(loc, TypeRange(), cond,
                                          /*addThenBlock=*/true,
                                          /*addElseBlock=*/static_cast<bool>(elseGen));

    auto fill = [&](Block* block, BodyGen& gen) {
        OpBuilder::InsertionGuard guard(builder);
        builder.setInsertionPointToEnd(block);
        const bool savedCfg = cfgAllowed;
        cfgAllowed = false;
        gen();
        cfgAllowed = savedCfg;
        builder.create<scf::YieldOp>(loc);
    };

    fill(&ifOp.getThenRegion().front(), thenGen);
    if (elseGen) fill(&ifOp.getElseRegion().front(), elseGen);
}

//   preBlock ──cond_br──▶ thenBlock ──▶ mergeBlock
//                    ╰──▶ elseBlock ──▶ mergeBlock   (elseBlock opsiyonel)
void MLIRControlFlowManager::buildIfCFG(Location loc, Value cond,
                                        BodyGen& thenGen, BodyGen& elseGen) {
    // Dışarı atlayan bir if'in, atladığı hedefle aynı (çok bloklu) bölgede
    // olması gerekir; Sema'nın işaretlemesi bunu garanti eder.
    assert(cfgAllowed && "bloklu if, yapisal (scf) bir bolgenin icinde uretilemez");

    OpBuilder& builder = ctx.builder;
    Block* preBlock = builder.getInsertionBlock();
    Region* region  = preBlock->getParent();

    // Blokları oluştur; createBlock insertion point'i değiştirdiği için guard kullan.
    Block* thenBlock;
    Block* elseBlock = nullptr;
    Block* mergeBlock;
    {
        OpBuilder::InsertionGuard guard(builder);
        thenBlock = builder.createBlock(region);
        if (elseGen)
            elseBlock = builder.createBlock(region);
        mergeBlock = builder.createBlock(region);
    }

    builder.setInsertionPointToEnd(preBlock);
    builder.create<cf::CondBranchOp>(
        loc, cond,
        thenBlock, ValueRange(),
        elseGen ? elseBlock : mergeBlock, ValueRange());

    // Dalın içinde iç içe kontrol akışı üretilmişse builder artık thenBlock'ta
    // olmayabilir; bu yüzden her zaman GÜNCEL bloğa bakılır.
    builder.setInsertionPointToEnd(thenBlock);
    thenGen();
    if (!isTerminated(builder.getInsertionBlock()))
        builder.create<cf::BranchOp>(loc, mergeBlock);
    Block* thenEnd = builder.getInsertionBlock();

    if (elseGen) {
        builder.setInsertionPointToEnd(elseBlock);
        elseGen();
        if (!isTerminated(builder.getInsertionBlock()))
            builder.create<cf::BranchOp>(loc, mergeBlock);
    }

    // Bütün dallar return/break/continue ile bittiyse merge bloğuna hiçbir
    // yerden gelinmez. Boş (terminator'sız) bir blok bırakmak yerine onu
    // siliyoruz; builder sonlanmış bir blokta kaldığı için çağıran taraf
    // devamındaki erişilemez kodu üretmez.
    if (mergeBlock->hasNoPredecessors()) {
        mergeBlock->erase();
        builder.setInsertionPointToEnd(thenEnd);
        return;
    }
    builder.setInsertionPointToEnd(mergeBlock);
}

// ===========================================================================
// while
// ===========================================================================

void MLIRControlFlowManager::buildWhile(Location loc, bool structured, bool hasContinue,
                                        CondGen condGen, BodyGen bodyGen) {
    if (!structured) {
        withCFG(loc, [&] { buildWhileCFG(loc, condGen, bodyGen); });
        return;
    }

    //   scf.while : () -> () {
    //     %cond = ...
    //     scf.condition(%cond)
    //   } do {
    //     ... gövde ...
    //     scf.yield
    //   }
    //
    // Değişkenler bellekte (memref) yaşadığı için döngünün taşıdığı SSA
    // değeri yoktur.
    ctx.builder.create<scf::WhileOp>(
        loc, TypeRange(), ValueRange(),
        [&](OpBuilder& b, Location l, ValueRange) {
            const bool savedCfg = cfgAllowed;
            cfgAllowed = false;
            Value cond = condGen();
            cfgAllowed = savedCfg;
            b.create<scf::ConditionOp>(l, cond, ValueRange());
        },
        [&](OpBuilder& b, Location l, ValueRange) {
            emitStructuredLoopBody(l, hasContinue, bodyGen);
            b.create<scf::YieldOp>(l);
        });
}

//   preBlock ──▶ headerBlock ──cond_br──▶ bodyBlock ──▶ headerBlock
//                           ╰──▶ exitBlock
void MLIRControlFlowManager::buildWhileCFG(Location loc, CondGen& condGen, BodyGen& bodyGen) {
    OpBuilder& builder = ctx.builder;
    Block* preBlock = builder.getInsertionBlock();
    Region* region  = preBlock->getParent();

    Block* headerBlock;
    Block* bodyBlock;
    Block* exitBlock;
    {
        OpBuilder::InsertionGuard guard(builder);
        headerBlock = builder.createBlock(region);
        bodyBlock   = builder.createBlock(region);
        exitBlock   = builder.createBlock(region);
    }

    builder.setInsertionPointToEnd(preBlock);
    builder.create<cf::BranchOp>(loc, headerBlock);

    // header: koşulu hesapla, koşullu atla. Koşul kendi içinde yapı
    // üretebileceği (kısa devre) için dal, builder'ın güncel bloğuna eklenir.
    builder.setInsertionPointToEnd(headerBlock);
    Value cond = condGen();
    builder.create<cf::CondBranchOp>(
        loc, cond,
        bodyBlock, ValueRange(),
        exitBlock, ValueRange());

    // body: 'continue' → header, 'break' → exit
    builder.setInsertionPointToEnd(bodyBlock);
    loopStack.push_back({headerBlock, exitBlock});
    bodyGen();
    if (!isTerminated(builder.getInsertionBlock()))
        builder.create<cf::BranchOp>(loc, headerBlock);
    loopStack.pop_back();

    builder.setInsertionPointToEnd(exitBlock);
}

// ===========================================================================
// Sayaçlı döngü (for-range ve for-collection)
// ===========================================================================

void MLIRControlFlowManager::buildCountedLoop(Location loc, Value lb, Value ub, Value step,
                                              bool isSigned, bool structured, bool hasContinue,
                                              IterBodyGen bodyGen) {
    // Bu MLIR sürümünde scf.for sınırı işaretli karşılaştırır; işaretsiz
    // sayaçlar bloklu yola düşer.
    if (!structured || !isSigned) {
        withCFG(loc, [&] { buildCountedLoopCFG(loc, lb, ub, step, isSigned, bodyGen); });
        return;
    }

    //   scf.for %iv = %lb to %ub step %step { ... gövde ... }
    ctx.builder.create<scf::ForOp>(
        loc, lb, ub, step, ValueRange(),
        [&](OpBuilder& b, Location l, Value iv, ValueRange) {
            emitStructuredLoopBody(l, hasContinue, [&] { bodyGen(iv); });
            b.create<scf::YieldOp>(l);
        });
}

//   preBlock ──▶ headerBlock ──cond_br──▶ bodyBlock ──▶ incrBlock ──▶ headerBlock
//                           ╰──▶ exitBlock
void MLIRControlFlowManager::buildCountedLoopCFG(Location loc, Value lb, Value ub, Value step,
                                                 bool isSigned, IterBodyGen& bodyGen) {
    OpBuilder& builder = ctx.builder;

    // Sayaç bellekte tutulur; gövdeye her yinelemede okunan değeri verilir.
    Value counter = ctx.createEntryAlloca(loc, MemRefType::get({}, lb.getType()));
    builder.create<memref::StoreOp>(loc, lb, counter);

    Block* preBlock = builder.getInsertionBlock();
    Region* region  = preBlock->getParent();

    Block* headerBlock;
    Block* bodyBlock;
    Block* incrBlock; // 'continue' buraya atlar: sayaç yine de artmalı
    Block* exitBlock;
    {
        OpBuilder::InsertionGuard guard(builder);
        headerBlock = builder.createBlock(region);
        bodyBlock   = builder.createBlock(region);
        incrBlock   = builder.createBlock(region);
        exitBlock   = builder.createBlock(region);
    }

    builder.setInsertionPointToEnd(preBlock);
    builder.create<cf::BranchOp>(loc, headerBlock);

    // header: iv < ub ?
    builder.setInsertionPointToEnd(headerBlock);
    Value iv = builder.create<memref::LoadOp>(loc, counter);
    Value inBounds = builder.create<arith::CmpIOp>(
        loc, isSigned ? arith::CmpIPredicate::slt : arith::CmpIPredicate::ult, iv, ub);
    builder.create<cf::CondBranchOp>(
        loc, inBounds,
        bodyBlock, ValueRange(),
        exitBlock, ValueRange());

    // body: 'continue' → incrBlock, 'break' → exitBlock
    builder.setInsertionPointToEnd(bodyBlock);
    loopStack.push_back({incrBlock, exitBlock});
    bodyGen(iv);
    if (!isTerminated(builder.getInsertionBlock()))
        builder.create<cf::BranchOp>(loc, incrBlock);
    loopStack.pop_back();

    // incr: iv += step, → header
    builder.setInsertionPointToEnd(incrBlock);
    Value next = builder.create<arith::AddIOp>(loc, iv, step);
    builder.create<memref::StoreOp>(loc, next, counter);
    builder.create<cf::BranchOp>(loc, headerBlock);

    builder.setInsertionPointToEnd(exitBlock);
}

// ===========================================================================
// Paralel döngü
// ===========================================================================

//   scf.parallel (%iv) = (%lb) to (%ub) step (%step) {
//     %yigit = llvm.intr.stacksave
//     scf.execute_region {
//       ^giris:  ... gövdenin yığıt tahsisleri ve gövde ...   (continue → ^son)
//       ^son:    scf.yield
//     }
//     llvm.intr.stackrestore %yigit
//   }
//
// scf.parallel "bu yinelemeler bağımsızdır" bilgisini IR'de taşır. İndirme
// hattı onu bugün CPU çekirdeklerine dağıtır (async); aynı operasyon ileride
// GPU çekirdeğine çevrilecek olan yapıdır.
//
// Yığıt: gövdede tanımlanan değişkenler her yinelemeye özel olmalıdır (aksi
// halde iş parçacıkları aynı yuvayı paylaşırdı), bu yüzden çağıran taraf
// gövdenin tahsislerini fonksiyon girişine değil execute_region'ın giriş
// bloğuna yapar. Döngü içindeki tahsis her yinelemede yığıtı büyüteceğinden,
// yığıtın konumu yineleme başında kaydedilir ve sonunda geri yüklenir.
// (memref.alloca_scope aynı işi yapar ama içinde döngü/koşul bulunamıyor.)
void MLIRControlFlowManager::buildParallelLoop(Location loc, Value lb, Value ub, Value step,
                                               IterBodyGen bodyGen) {
    OpBuilder& builder = ctx.builder;
    builder.create<scf::ParallelOp>(
        loc, ValueRange(lb), ValueRange(ub), ValueRange(step),
        [&](OpBuilder& b, Location l, ValueRange ivs) {
            Value stack = b.create<LLVM::StackSaveOp>(l, LLVM::LLVMPointerType::get(&ctx.context));

            // Paralel gövdeden dışarıya atlanamaz; yeni bir döngü bağlamı başlar.
            // Gövde her zaman execute_region içinde üretilir: hem tahsislere
            // yer açar hem de `continue`yu karşılar.
            auto savedLoops = std::move(loopStack);
            loopStack.clear();
            emitStructuredLoopBody(l, /*hasContinue=*/true, [&] { bodyGen(ivs[0]); });
            loopStack = std::move(savedLoops);

            b.create<LLVM::StackRestoreOp>(l, stack);
        });
}

// ===========================================================================
// Kısa devreli && / ||
// ===========================================================================

//   a && b  →  %r = scf.if %a -> i1 { yield %b } else { yield false }
//   a || b  →  %r = scf.if %a -> i1 { yield true } else { yield %b }
Value MLIRControlFlowManager::buildShortCircuit(Location loc, Value lhs, bool isAnd,
                                                CondGen rhsGen) {
    OpBuilder& builder = ctx.builder;
    Type resultTypes[] = {builder.getI1Type()};
    auto ifOp = builder.create<scf::IfOp>(loc, TypeRange(resultTypes), lhs,
                                          /*addThenBlock=*/true, /*addElseBlock=*/true);

    auto fill = [&](Block* block, bool evaluateRhs) {
        OpBuilder::InsertionGuard guard(builder);
        builder.setInsertionPointToEnd(block);
        Value result;
        if (evaluateRhs) {
            const bool savedCfg = cfgAllowed;
            cfgAllowed = false;
            result = rhsGen();
            cfgAllowed = savedCfg;
        } else {
            result = builder.create<arith::ConstantIntOp>(loc, isAnd ? 0 : 1, 1);
        }
        builder.create<scf::YieldOp>(loc, ValueRange(result));
    };

    fill(&ifOp.getThenRegion().front(), /*evaluateRhs=*/isAnd);
    fill(&ifOp.getElseRegion().front(), /*evaluateRhs=*/!isAnd);
    return ifOp.getResult(0);
}
