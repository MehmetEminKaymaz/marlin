//
// Created by Eminf on 19.07.2026.
//

#ifndef MLIRCONTROLFLOWMANAGER_H
#define MLIRCONTROLFLOWMANAGER_H

#include "CodegenContext.h"
#include "llvm/ADT/STLFunctionalExtras.h"
#include "llvm/ADT/SmallVector.h"
#include <functional>

// ---------------------------------------------------------------------------
// MLIRControlFlowManager
//
// Akış kontrolünü HİBRİT olarak üretir:
//
//   * Yapısal yol (scf): gövdesinden dışarı atlamayan her if ve döngü
//     scf.if / scf.while / scf.for olarak üretilir. Bu operasyonlar döngü
//     yapısını (sınırlar, adım, gövde) IR'de açıkça taşıdığı için ileride
//     paralelleştirme ve GPU'ya indirme (scf.parallel → gpu) bunların
//     üzerinden yapılır.
//
//   * Bloklu yol (cf): `break` ya da `return` içeren döngüler ile dışarı
//     atlayan if'ler, blok + dal (cf.br / cf.cond_br) olarak üretilir. scf
//     bölgeleri tek bloklu olduğundan bu atlamalar orada ifade edilemez.
//
// Hangi yolun seçileceğine Sema karar verir (IfStmt::needsCFG,
// LoopStmt::needsCFG / hasContinue); bu sınıf yalnızca `structured` bayrağına
// bakar.
//
// İki yol şöyle iç içe geçer: bir scf bölgesinin içinde bloklu bir yapı
// gerekirse (ör. scf.for içinde break'li bir while) o yapı, çok bloklu
// bölgeye izin veren scf.execute_region içine sarılır; dıştaki döngü yapısal
// kalır. `continue` de aynı mekanizmayla çözülür: döngü scf.for olarak kalır,
// gövdesi bir execute_region'a konur ve continue, bölgenin son bloğuna atlar.
//
//   * Paralel yol (scf.parallel): `parallel for` döngüleri. Yinelemelerin
//     bağımsız olduğu bilgisi IR'de açıkça durur.
// ---------------------------------------------------------------------------
class MLIRControlFlowManager {
public:
    using BodyGen     = std::function<void()>;
    using CondGen     = std::function<mlir::Value()>;
    using IterBodyGen = std::function<void(mlir::Value iv)>;

private:
    CodegenContext& ctx;

    // Aktif döngünün continue ve break hedef blokları.
    struct LoopContext {
        mlir::Block* continueBlock; // 'continue' → bu bloğa atla
        mlir::Block* exitBlock;     // 'break'    → bu bloğa atla (yapısal döngüde yoktur)
    };

    llvm::SmallVector<LoopContext, 8> loopStack;

    // O an üretim yapılan bölge yeni blok kabul ediyor mu? Fonksiyon gövdesi
    // ve scf.execute_region için true, scf.if/for/while gövdeleri için false.
    bool cfgAllowed = true;

public:
    explicit MLIRControlFlowManager(CodegenContext& context) : ctx(context) {}

    /// Blok bir terminator ile bitiyor mu (arkasına kod eklenemez mi)?
    static bool isTerminated(mlir::Block* block);

    // -----------------------------------------------------------------------
    // Fonksiyon sınırları: her fonksiyon/lambda kendi döngü yığınıyla başlar.
    // -----------------------------------------------------------------------
    struct State {
        llvm::SmallVector<LoopContext, 8> loopStack;
        bool cfgAllowed;
    };
    State enterFunction();
    void leaveFunction(State saved);

    // -----------------------------------------------------------------------
    // Yapılar
    // -----------------------------------------------------------------------

    /// if / else. `elseGen` boş olabilir.
    void buildIf(mlir::Location loc, mlir::Value cond, bool structured,
                 BodyGen thenGen, BodyGen elseGen);

    /// while. Koşul her yinelemede `condGen` ile yeniden üretilir.
    void buildWhile(mlir::Location loc, bool structured, bool hasContinue,
                    CondGen condGen, BodyGen bodyGen);

    /// Sayaçlı döngü: iv = lb; iv < ub; iv += step  (step > 0 varsayılır).
    /// lb/ub/step aynı tipte (iN ya da index) olmalıdır. `bodyGen`e sayaç SSA
    /// değeri olarak verilir.
    void buildCountedLoop(mlir::Location loc, mlir::Value lb, mlir::Value ub, mlir::Value step,
                          bool isSigned, bool structured, bool hasContinue,
                          IterBodyGen bodyGen);

    /// Paralel döngü: yinelemeler birbirinden bağımsızdır ve aynı anda
    /// çalışabilir. lb/ub/step index tipinde olmalıdır. Sema, gövdenin buna
    /// uygun olduğunu doğrulamıştır.
    void buildParallelLoop(mlir::Location loc, mlir::Value lb, mlir::Value ub, mlir::Value step,
                           IterBodyGen bodyGen);

    /// Kısa devreli && / ||: sağ taraf yalnızca gerekirse hesaplanır.
    mlir::Value buildShortCircuit(mlir::Location loc, mlir::Value lhs, bool isAnd,
                                  CondGen rhsGen);

    void emitBreak(mlir::Location loc);
    void emitContinue(mlir::Location loc);

private:
    // Bloklu (cf) yol
    void buildIfCFG(mlir::Location loc, mlir::Value cond, BodyGen& thenGen, BodyGen& elseGen);
    void buildWhileCFG(mlir::Location loc, CondGen& condGen, BodyGen& bodyGen);
    void buildCountedLoopCFG(mlir::Location loc, mlir::Value lb, mlir::Value ub, mlir::Value step,
                             bool isSigned, IterBodyGen& bodyGen);

    // Bloklu bir yapıyı, gerekiyorsa scf.execute_region içine sararak üretir.
    void withCFG(mlir::Location loc, llvm::function_ref<void()> emit);

    // Yapısal bir döngünün (tek bloklu) gövdesini üretir.
    void emitStructuredLoopBody(mlir::Location loc, bool hasContinue,
                                llvm::function_ref<void()> bodyGen);
};

#endif // MLIRCONTROLFLOWMANAGER_H
