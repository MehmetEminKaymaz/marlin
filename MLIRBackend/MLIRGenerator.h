//
// Created by Eminf on 19.07.2026.
//

#ifndef MLIRGENERATOR_H
#define MLIRGENERATOR_H

#include "../AST/ASTNode.h"
#include "CodegenContext.h"
#include "MLIRTypeManager.h"
#include "MLIRScopeManager.h"
#include "MLIRArithManager.h"
#include "MLIRControlFlowManager.h"
#include "MLIRBuiltinManager.h"
#include "MLIRListManager.h"
#include "MLIRMatrixManager.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/STLFunctionalExtras.h"
#include "mlir/Dialect/Func/IR/FuncOps.h"
#include <string>
#include <unordered_map>

// ---------------------------------------------------------------------------
// MLIRGenerator
//
// Sema'dan geçmiş (tipleri çözülmüş, dönüşümleri açıkça yazılmış) AST'yi MLIR'e
// çevirir. Kendi başına karar vermez: tipleri Expression::type'tan, isimlerin
// neye çözüldüğünü Identifier::ref'ten, akış yapısını needsCFG bayraklarından
// okur. İşin parçaları yardımcı sınıflara dağıtılmıştır:
//
//   MLIRTypeManager         SemType → mlir::Type
//   MLIRScopeManager        yerel isim → değer tablosu
//   MLIRArithManager        aritmetik, karşılaştırma, dönüşüm, sabitler
//   MLIRControlFlowManager  if / döngüler (scf ve cf)
//   MLIRBuiltinManager      yerleşikler: print (runtime çağrısı), matematik
//   MLIRListManager         list<T>: oluşturma, erişim, push / pop
//   MLIRMatrixManager       matrix<T>: tensor üzerinde işlemler, eleman erişimi
//   MLIRRuntime             çalışma zamanı kütüphanesine çağrılar
//
// Uygulama dört dosyaya bölünmüştür:
//   MLIRGenerator.cpp        program, fonksiyonlar, globaller, bellek yardımcıları
//   MLIRGeneratorExpr.cpp    ifadeler
//   MLIRGeneratorStmt.cpp    deyimler
//   MLIRGeneratorMatrix.cpp  matris ifadeleri ve matris belleğinin sahipliği
//
// Değerlerin gösterimi:
//   sayısal / bool / char  → SSA değeri; değişkense memref<T> yuvasında
//   dizi  T[N]             → memref<N x T> (başvuru olarak dolaşır)
//   liste list<T>          → memref<memref<? x T>> (yığın belleğindeki hücre;
//                            başvuru olarak dolaşır, değişkense yuvada tutulur)
//   matris matrix<T>       → ifade değeri olarak tensor<? x ? x T>; değişkende,
//                            parametrede ve dönüşte memref<? x ? x T>. Değer
//                            olarak davranır; belleğinin tek bir sahibi vardır.
//   string                 → {ptr, i64} struct; değişkense llvm.alloca yuvasında
//   fonksiyon / lambda     → fonksiyon tipli SSA değeri (func.constant)
//
// GPU
//
// @GPU fonksiyon da sıradan bir fonksiyon olarak üretilir; farkları şunlardır:
//
//   * Matris belleği GPU'dan ayrılır (gpu.alloc / gpu.dealloc). Matrislerin
//     tipi değişmez (memref); yalnızca gösterdikleri bellek GPU'dadır.
//   * Fonksiyon `minik.gpu` ile işaretlenir. İndirme hattı işaretli
//     fonksiyonlardaki paralel döngüleri (`parallel for` ve matris işlemleri)
//     GPU çekirdeklerine çevirir (bkz. LLVMBackend/GpuOffload). Kod üretimi
//     CPU ile aynı IR'yi üretir; hedefi indirme hattı belirler.
//   * `parallel for` gövdesi GPU'da çalışacağı için orada çalışma zamanı
//     kütüphanesi çağrılmaz (indeks sınır denetimi yapılmaz).
//   * Parametre ya da dönüşünde matris varsa, CPU'dan çağrılabilmesi için
//     ayrıca bir giriş fonksiyonu (`ad.host`) üretilir: matrisleri GPU'ya
//     kopyalar, asıl fonksiyonu çağırır, sonucu CPU'ya kopyalar.
//
// GPU hedefi kapalıysa (--gpu=off) bunların hiçbiri yapılmaz; @GPU fonksiyon
// CPU fonksiyonu olarak üretilir.
// ---------------------------------------------------------------------------
class MLIRGenerator : public ASTVisitor {
    CodegenContext        ctx;
    MLIRTypeManager       typeManager;
    MLIRScopeManager      scopeManager;
    MLIRArithManager      arithManager;
    MLIRControlFlowManager cfManager;
    MLIRRuntime           runtime;
    MLIRBuiltinManager    builtinManager;
    MLIRListManager       listManager;
    MLIRMatrixManager     matrixManager;

    mlir::Value lastValue; // son ziyaret edilen ifadenin değeri
    int         stringLiteralCount = 0;
    int         lambdaCounter      = 0;
    bool        usesParallel       = false;
    bool        usesGpu            = false;
    // Programdaki fonksiyonlar (çağrı üretilirken hedefleri buradan okunur).
    std::unordered_map<std::string, FunctionDecl*> functions;

    // --- Matris sahipliği (ayrıntı: MLIRGeneratorMatrix.cpp) ---
    // Üretilmekte olan deyimde hesaplanmış, henüz kimsenin sahiplenmediği
    // matris bellekleri. Deyim bitince bırakılırlar (flushTemporaries).
    llvm::SmallVector<mlir::Value, 8> temporaries;
    // tensor → arkasındaki sahipsiz bellek (yalnızca geçiciler için)
    llvm::DenseMap<mlir::Value, mlir::Value> ownedBuffers;
    // Açık her blok için, o blokta tanımlanan matris değişkenlerinin yuvaları.
    // Bloktan çıkılırken (normal, return, break, continue) bellekleri bırakılır.
    std::vector<std::vector<mlir::Value>> matrixScopes;
    // İçinde bulunulan döngülerin gövdesi başladığında matrixScopes'un boyutu:
    // break/continue bu derinlikten sonrasını bırakır.
    std::vector<size_t> loopScopeDepths;

public:
    MLIRGenerator(mlir::MLIRContext& context, mlir::OpBuilder& builder, mlir::ModuleOp moduleOp)
        : ctx(builder, context, moduleOp),
          typeManager(ctx), scopeManager(ctx),
          arithManager(ctx), cfManager(ctx),
          runtime(ctx),
          builtinManager(ctx, arithManager, runtime),
          listManager(ctx, runtime),
          matrixManager(ctx, runtime) {}

    /// Program `parallel for` içeriyor mu? (İçeriyorsa çalıştırılabilir dosyaya
    /// iş parçacığı çalışma zamanı da bağlanmalıdır.)
    bool usesParallelLoops() const { return usesParallel; }

    /// @GPU fonksiyonlar GPU için mi üretilsin? (Varsayılan: evet.)
    void setGpuEnabled(bool enabled) { ctx.gpuEnabled = enabled; }
    /// Program GPU'da çalışan kod içeriyor mu? (İçeriyorsa indirme hattına GPU
    /// adımları eklenir ve çalıştırılabilir dosyaya GPU çalışma zamanı bağlanır.)
    bool usesGpuCode() const { return usesGpu; }

    void visit(Program* node) override;
    void visit(Type* node) override;

    // İfadeler
    void visit(IntegerLiteral* node) override;
    void visit(FloatLiteral* node) override;
    void visit(BooleanLiteral* node) override;
    void visit(CharLiteral* node) override;
    void visit(StringLiteral* node) override;
    void visit(Identifier* node) override;
    void visit(ListLiteral* node) override;
    void visit(ArrayAccessExpr* node) override;
    void visit(FunctionCallExpr* node) override;
    void visit(UnaryExpr* node) override;
    void visit(BinaryExpr* node) override;
    void visit(CastExpr* node) override;
    void visit(LambdaExpr* node) override;

    // Deyimler
    void visit(BlockStmt* node) override;
    void visit(ExprStmt* node) override;
    void visit(VariableDecl* node) override;
    void visit(AssignmentStmt* node) override;
    void visit(ParamDecl* node) override;
    void visit(FunctionDecl* node) override;
    void visit(IfStmt* node) override;
    void visit(WhileStmt* node) override;
    void visit(ForRangeStmt* node) override;
    void visit(ForCollectionStmt* node) override;
    void visit(BreakStmt* node) override;
    void visit(ContinueStmt* node) override;
    void visit(ReturnStmt* node) override;

private:
    mlir::Location locOf(const ASTNode* node) { return ctx.getLoc(node->line, node->column); }

    /// İfadeyi üretir ve değerini döndürür.
    mlir::Value emitExpr(Expression* expr) {
        expr->accept(*this);
        return lastValue;
    }

    // --- Fonksiyonlar ve globaller (MLIRGenerator.cpp) ---
    void declareFunctionSignature(FunctionDecl* node);
    /// Fonksiyon GPU için mi üretiliyor?
    bool targetsGpu(const FunctionDecl* node) const { return node->isGpu && ctx.gpuEnabled; }
    /// @GPU fonksiyonun CPU'dan çağrılan giriş fonksiyonunun adı.
    static std::string hostEntryName(const std::string& name) { return name + ".host"; }
    void emitHostEntry(FunctionDecl* node);
    void emitGlobalVariable(VariableDecl* node);
    /// Fonksiyon ve lambdaların ortak gövde üretimi: giriş bloğu, parametreler,
    /// fonksiyona özgü durumun (kapsam, döngü yığını) kurulup geri alınması.
    void emitFunctionBody(mlir::func::FuncOp funcOp, const SemType& fnType,
                          const std::vector<std::unique_ptr<ParamDecl>>& params,
                          mlir::Location loc, llvm::function_ref<void()> emitBody);
    mlir::Value emitGlobalRef(const std::string& name, mlir::Location loc);
    mlir::Attribute constantAttr(Expression* expr, const SemType& type);

    // --- Bellek (MLIRGenerator.cpp) ---
    /// `type` tipinde bir değişken için fonksiyon girişinde yer ayırır.
    mlir::Value createStorage(const SemType& type, mlir::Location loc);
    void storeTo(mlir::Value value, mlir::Value storage, const SemType& type, mlir::Location loc);
    mlir::Value loadFrom(mlir::Value storage, const SemType& type, mlir::Location loc);
    mlir::Value emitZero(const SemType& type, mlir::Location loc);
    mlir::Value toIndex(mlir::Value value, const SemType& type, mlir::Location loc);
    /// Liste literalinin elemanlarını sırayla `memref`e yazar.
    void emitListInto(ListLiteral* list, mlir::Value memref);

    // --- Yerleşikler (MLIRGeneratorExpr.cpp) ---
    void emitBuiltinCall(FunctionCallExpr* node);

    // --- Matrisler (MLIRGeneratorMatrix.cpp) ---
    struct MatrixExprNode;
    struct MatrixExprPlan;
    /// Eleman eleman matris ifadesini (a + b * 2, -a, tip dönüşümü ...) tek
    /// bir işlem olarak üretir.
    void emitMatrixElementwise(Expression* node);
    std::unique_ptr<MatrixExprNode> planMatrixExpr(Expression* expr, MatrixExprPlan& plan);
    mlir::Value evalMatrixExpr(const MatrixExprNode& node, mlir::ValueRange elements,
                               mlir::Location loc);
    void emitMatrixLiteral(ListLiteral* node);
    void emitMatrixBuiltin(FunctionCallExpr* node);
    void emitMatrixPrint(Expression* matrix, mlir::Location loc);
    /// Matris ifadesinin belleği (okumak için). Değişkense doğrudan onun
    /// belleği, değilse hesaplanan geçicinin belleği.
    mlir::Value emitMatrixBuffer(Expression* expr);

    /// Yeni hesaplanmış matrisi geçici olarak kaydeder ve lastValue yapar.
    void setMatrixResult(MLIRMatrixManager::Result result);
    /// İfadenin sonucunu saklamak üzere sahiplenir: yeni hesaplanmışsa belleği
    /// devralır, bir değişkene aitse kopyasını çıkarır.
    mlir::Value takeMatrixOwnership(mlir::Value tensor, int line, mlir::Location loc);
    /// `from` indeksinden sonraki geçici bellekleri bırakır.
    void flushTemporaries(mlir::Location loc, size_t from = 0);
    /// `fromDepth` ve daha içteki blokların matris değişkenlerini bırakır
    /// (`keep` yuvası hariç: return ile dışarı taşınan değişken).
    void freeMatrixScopes(size_t fromDepth, mlir::Location loc, mlir::Value keep = {});
    /// İsim, bu fonksiyonda tanımlı (sahibi olduğumuz) bir matris değişkeniyse yuvası.
    mlir::Value ownedMatrixSlot(Expression* expr);

    // --- Deyimler (MLIRGeneratorStmt.cpp) ---
    /// Deyimleri sırayla üretir; return/break/continue'dan sonrasını (erişilemez
    /// kodu) üretmez.
    void emitStatements(const std::vector<std::unique_ptr<Statement>>& statements);
    std::function<void()> buildElseChain(IfStmt* node, size_t idx);
    void emitLoopBody(BlockStmt* body);
    void emitParallelFor(ForRangeStmt* node, mlir::Value start, mlir::Value end, mlir::Value step);
};

#endif // MLIRGENERATOR_H
