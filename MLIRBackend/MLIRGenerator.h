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
#include "mlir/Interfaces/FunctionInterfaces.h"
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
// Uygulama sekiz dosyaya bölünmüştür:
//   MLIRGenerator.cpp        program, fonksiyonlar, globaller, bellek yardımcıları
//   MLIRGeneratorExpr.cpp    ifadeler
//   MLIRGeneratorStmt.cpp    deyimler
//   MLIRGeneratorMatrix.cpp  matris ifadeleri ve matris belleğinin sahipliği
//   MLIRGeneratorStruct.cpp  struct değerleri: oluşturma, kopyalama, alan erişimi
//   MLIRGeneratorList.cpp    liste elemanları: push / pop / free, struct listesi
//   MLIRGeneratorString.cpp  stringlerin belleği: başvuru sayımı
//   MLIRGeneratorTask.cpp    görevler: marl, await, coroutine fonksiyonlar
//
// Değerlerin gösterimi:
//   sayısal / bool / char  → SSA değeri; değişkense memref<T> yuvasında
//   dizi  T[N]             → memref<N x T> (başvuru olarak dolaşır)
//   liste list<T>          → memref<memref<? x T>> (yığın belleğindeki hücre;
//                            başvuru olarak dolaşır, değişkense yuvada tutulur)
//   matris matrix<T>       → ifade değeri olarak tensor<? x ? x T>; değişkende,
//                            parametrede ve dönüşte memref<? x ? x T>. Değer
//                            olarak davranır; belleğinin tek bir sahibi vardır.
//   string                 → i64: çalışma zamanındaki string bloğunun adresi
//                            (değişmez; işlemleri runtime çağrılarıdır; belleği
//                            başvuru sayımıyla kendiliğinden bırakılır).
//                            Skaler gibi saklanır: değişkense memref<i64> yuvasında
//   struct                 → tek bir değer değildir: yapraklarına (struct
//                            olmayan alanlarına) ayrılır. `Nokta p` değişkeni
//                            "p.x", "p.y" adlı ayrı değişkenlerdir; her biri
//                            kendi tipinin gösterimini kullanır. Fonksiyona
//                            yaprak yaprak verilir ve döner
//                            (bkz. MLIRGeneratorStruct.cpp)
//   struct listesi         → o da yapraklarına ayrılır: list<Nokta> l, alan
//                            başına bir listedir ("l.x", "l.y")
//                            (bkz. MLIRGeneratorList.cpp)
//   fonksiyon / lambda     → i64: fonksiyonun adresi (skaler gibi saklanır;
//                            bkz. functionAddress)
//   görev task<T>          → i64: runtime'daki görev nesnesinin adresi
//                            (string gibi başvuru sayılır; bkz. MLIRGeneratorTask.cpp)
//
// GPU
//
// @GPU fonksiyon da sıradan bir fonksiyon olarak üretilir; farkları şunlardır:
//
//   * Matris belleği GPU'dan ayrılır (gpu.alloc / gpu.dealloc). Matrislerin
//     tipi değişmez (memref); yalnızca gösterdikleri bellek GPU'dadır.
//   * Fonksiyon `marlin.gpu` ile işaretlenir. İndirme hattı işaretli
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
    bool        usesTasks          = false;
    // Üretilmekte olan fonksiyon bir coroutine (async.func) mu? main mi?
    bool        emittingAsync      = false;
    bool        emittingMain       = false;
    // Programdaki fonksiyonlar (çağrı üretilirken hedefleri buradan okunur).
    std::unordered_map<std::string, FunctionDecl*> functions;
    // Derlenen program; konum bilgisine dosya adını yazmak için.
    const Program* program = nullptr;

    // --- Sahiplik: matrisler ve stringler ---
    // (ayrıntı: MLIRGeneratorMatrix.cpp, MLIRGeneratorString.cpp)
    //
    // Sahibi olan bir değer. `type` ne olduğunu ve nasıl bırakılacağını söyler:
    //   matris        geçicide belleğin kendisi, değişkende belleği tutan yuva
    //   string        geçicide değerin kendisi, değişkende değeri tutan yuva
    //   string dizisi dizinin belleği (elemanlarının her biri bırakılır)
    struct Owned {
        mlir::Value value;
        SemType type;
    };
    // Üretilmekte olan deyimde hesaplanmış, henüz kimsenin sahiplenmediği
    // değerler. Deyim bitince bırakılırlar (flushTemporaries).
    llvm::SmallVector<Owned, 8> temporaries;
    // tensor → arkasındaki sahipsiz bellek (yalnızca matris geçicileri için)
    llvm::DenseMap<mlir::Value, mlir::Value> ownedBuffers;
    // Açık her blok için, o blokta tanımlanan matris ve string değişkenleri.
    // Bloktan çıkılırken (normal, return, break, continue) bırakılırlar. İlk
    // sıra fonksiyonun kendisidir (string parametreler).
    std::vector<std::vector<Owned>> ownedScopes;
    // İçinde bulunulan döngülerin gövdesi başladığında ownedScopes'un boyutu:
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

    /// Program CPU'da paralel çalışan kod (`parallel for` ya da matris çarpımı)
    /// içeriyor mu? (İçeriyorsa çalıştırılabilir dosyaya iş parçacığı çalışma
    /// zamanı da bağlanmalıdır.)
    bool usesParallelLoops() const { return usesParallel || usesTasks; }
    /// Program `marl` ile görev başlatıyor mu? (Başlatıyorsa main bittiğinde
    /// süreç, çalışan görevleri beklemeden kapanmalıdır; bkz. LLVMLowering.)
    bool usesTaskCode() const { return usesTasks; }

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
    void visit(MemberExpr* node) override;
    void visit(MarlExpr* node) override;
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
    mlir::Location locOf(const ASTNode* node) {
        return ctx.getLoc(program->modules[node->module].path, node->line, node->column);
    }

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
    /// Sıfırla başlayan tek değerlik global (struct yaprağı, string).
    void emitZeroGlobal(const std::string& name, const SemType& type, mlir::Location loc);
    /// String tutan globaller: main dönerken bırakılırlar.
    std::vector<std::string> globalStrings;
    /// Fonksiyon ve lambdaların ortak gövde üretimi: giriş bloğu, parametreler,
    /// fonksiyona özgü durumun (kapsam, döngü yığını) kurulup geri alınması.
    void emitFunctionBody(mlir::FunctionOpInterface funcOp, const SemType& fnType,
                          const std::vector<std::unique_ptr<ParamDecl>>& params,
                          mlir::Location loc, llvm::function_ref<void()> emitBody);
    /// func.return ya da (coroutine'de) async.return.
    void emitReturnOp(mlir::ValueRange values, mlir::Location loc);
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
    /// `fromDepth` ve daha içteki blokların matris ve string değişkenlerini bırakır
    /// (`keep` yuvası hariç: return ile dışarı taşınan değişken).
    void freeScopes(size_t fromDepth, mlir::Location loc, mlir::Value keep = {});
    /// İsim, bu fonksiyonda tanımlı (sahibi olduğumuz) bir matris değişkeniyse yuvası.
    mlir::Value ownedMatrixSlot(Expression* expr);

    // --- Stringler ve görevler: başvuru sayımı (MLIRGeneratorString.cpp) ---
    //
    // String ve task<T> değerleri, çalışma zamanındaki başvuru sayılan bir
    // nesnenin adresidir (i64). Sahiplik kuralları ikisinde aynıdır; yalnızca
    // sayacı değiştiren runtime fonksiyonları farklıdır. Aşağıdaki "handle"
    // yardımcıları ikisi için de çalışır.
    static bool isHandle(const SemType& type) { return type.isString() || type.isTask(); }
    /// Bırakılması gereken bir handle ya da string dizisi mi?
    static bool holdsStrings(const SemType& type) {
        return isHandle(type) || (type.isArray() && type.elementType().isString());
    }
    void retainString(mlir::Value value, mlir::Location loc);
    void releaseString(mlir::Value value, mlir::Location loc);
    void retainHandle(mlir::Value value, const SemType& type, mlir::Location loc);
    void releaseHandle(mlir::Value value, const SemType& type, mlir::Location loc);
    /// Yeni üretilmiş (başvurusu bu deyime ait) stringi geçici olarak kaydeder.
    mlir::Value stringResult(mlir::Value value);
    mlir::Value handleResult(mlir::Value value, const SemType& type);
    /// Stringi saklamak üzere sahiplenir: deyimin geçicisiyse başvurusunu
    /// devralır, başkasınınsa sayacını artırır.
    mlir::Value ownString(mlir::Value value, mlir::Location loc);
    mlir::Value ownHandle(mlir::Value value, const SemType& type, mlir::Location loc);
    /// Diziyi kopyalar. String dizisinde her elemanın sayacı artar;
    /// `releaseOld`: hedefin eski elemanları bırakılır (hedef başlatılmışsa).
    void copyArray(mlir::Value source, mlir::Value target, const SemType& type, bool releaseOld,
                   mlir::Location loc);
    void releaseStringArray(mlir::Value array, const SemType& type, mlir::Location loc);
    void releaseOwned(const Owned& owned, bool isSlot, mlir::Location loc);
    /// 0 .. count arası her indeks için gövdeyi üretir (scf.for).
    void forEachIndex(int64_t count, mlir::Location loc,
                      llvm::function_ref<void(mlir::Value index)> body);

    // --- Struct'lar ve struct listeleri (MLIRGeneratorStruct.cpp) ---
    using Values = llvm::SmallVector<mlir::Value, 8>;
    /// Bileşik (struct ya da struct listesi) ifadeyi üretir; yapraklarının
    /// değerlerini tanım sırasıyla döndürür (her biri, o tipte bir ifadeyi
    /// emitExpr'in üreteceği biçimde). Bileşik olmayan ifadede tek değer döner.
    Values emitLeaves(Expression* expr);
    /// Yalnızca [begin, begin + count) aralığındaki yapraklar.
    Values emitLeaves(Expression* expr, size_t begin, size_t count);
    /// Parçası seçilemeyen ifadeler (oluşturma, çağrı, literal): bütün yapraklar.
    Values emitWholeLeaves(Expression* expr);
    /// f().x — tabandaki struct'ın bir alanına düşen yapraklar.
    Values emitMember(MemberExpr* node);
    static size_t leafOffset(const MemberExpr* node);

    // --- Listeler (MLIRGeneratorList.cpp) ---
    /// Değeri listede saklanacak hale getirir (matris ve dizi için listeye ait
    /// yeni bir bellek).
    mlir::Value adoptElement(mlir::Value value, const SemType& type, int line, mlir::Location loc);
    /// Listede saklanan elemanın ifade olarak değeri (ödünç: bellek listenindir).
    mlir::Value borrowElement(mlir::Value stored, const SemType& type, mlir::Location loc);
    void releaseElement(mlir::Value stored, const SemType& type, mlir::Location loc);
    /// liste[i] — elemanın [begin, begin + count) yaprakları.
    Values emitElement(ArrayAccessExpr* node, size_t begin, size_t count);
    Values emitStructListLiteral(ListLiteral* node);
    void emitListPush(FunctionCallExpr* node);
    Values emitListPop(FunctionCallExpr* node);
    void emitListFree(FunctionCallExpr* node);
    void freeListValue(mlir::Value cell, const SemType& listType, mlir::Location loc);
    /// Fonksiyona doğrudan verilen ve ödünç alınan liste literalini (Sema:
    /// ListLiteral::freeAfterCall) deyim sonunda bırakılmak üzere kaydeder.
    void ownTemporaryList(Expression* arg, const Values& cells);
    /// liste[i] = ...;   liste[i].alan = ...;
    void assignElement(AssignmentStmt* node);
    /// for p in liste — elemanı struct olan liste üzerinde.
    void emitStructListLoop(ForCollectionStmt* node);
    void declareStruct(VariableDecl* node);
    void assignStruct(AssignmentStmt* node, Identifier* target);
    /// Üretilmekte olan fonksiyon struct döndürüyorsa, dizi alanlarının
    /// yazılacağı yerler (çağıran ayırır; bkz. MLIRTypeManager::convertFunction).
    std::vector<mlir::Value> returnArraySlots;

    // --- Çağrılar (MLIRGeneratorExpr.cpp) ---
    /// Kullanıcı fonksiyonuna çağrıyı üretir. Sonuçlar: dönüş tipinin yaprakları
    /// sırasıyla (struct değilse tek değer, void ise boş).
    Values emitCall(FunctionCallExpr* node);
    /// Argümanları hazır çağrının kendisi (func.call / async.call /
    /// call_indirect). `calleeValue`: dolaylı çağrıda fonksiyonun adresi.
    /// Sonuçlar ham haliyle döner; geçici kaydı (stringResult ...) çağıranın işidir.
    Values emitCallOp(FunctionCallExpr* node, mlir::Value calleeValue,
                      llvm::ArrayRef<mlir::Value> args, mlir::Location loc);

    // --- scan (MLIRGeneratorScan.cpp) ---
    /// scan(rows): satırları struct'a / struct listesine doldurur; yapraklar döner.
    Values emitScan(FunctionCallExpr* node);
    /// Fonksiyon değerini (adres, i64) çağırır; tek sonucu döner (void ise boş).
    mlir::Value callFunctionValue(mlir::Value function, const SemType& type,
                                  llvm::ArrayRef<mlir::Value> args, mlir::Location loc);

    // --- Görevler (MLIRGeneratorTask.cpp) ---
    /// await(t)
    void emitAwait(FunctionCallExpr* node);
    /// Görevin kullanacağı, ona ait kopya (bkz. Sema.h, görev kuralı 1).
    mlir::Value copyForTask(mlir::Value value, const SemType& type, int line, mlir::Location loc);
    /// Görev bitince kopyayı bırakır.
    void releaseTaskCopy(mlir::Value copy, const SemType& type, mlir::Location loc);
    /// list<T>'nin yeni (elemanları kopyalanmış) bir kopyası.
    mlir::Value cloneList(mlir::Value list, const SemType& listType, mlir::Location loc);
    /// Runtime'daki görev fonksiyonunun, `asyncType` için bildirilmiş adı
    /// (bkz. MLIRGeneratorTask.cpp, "Tipli köprüler").
    std::string taskBridge(llvm::StringRef base, mlir::Type asyncType);
    llvm::DenseMap<mlir::Type, unsigned> taskBridgeIds;
    int taskBodyCounter = 0;
    /// `symbol` adlı fonksiyonun değer olarak kullanımı: adresi (i64).
    mlir::Value functionAddress(const std::string& symbol, const SemType& type, mlir::Location loc);
    /// extern fonksiyona (çalışma zamanı kütüphanesindeki C fonksiyonuna) çağrı.
    Values emitExternCall(FunctionDecl* decl, FunctionCallExpr* node);
    mlir::Type externType(const SemType& type);

    // --- Deyimler (MLIRGeneratorStmt.cpp) ---
    /// `name` adında yerel bir değişken oluşturur ve `value` ile başlatır.
    void declareLocal(const std::string& name, const SemType& type, mlir::Value value,
                      int line, mlir::Location loc);
    /// Başlangıç değeri verilmemiş yerel değişken.
    void declareDefault(const std::string& name, const SemType& type, mlir::Location loc);
    /// `return expr;` — fonksiyon ve tek ifadeli lambda gövdeleri için ortak.
    void emitReturn(Expression* expr, int line, mlir::Location loc);
    /// Deyimleri sırayla üretir; return/break/continue'dan sonrasını (erişilemez
    /// kodu) üretmez.
    void emitStatements(const std::vector<std::unique_ptr<Statement>>& statements);
    std::function<void()> buildElseChain(IfStmt* node, size_t idx);
    /// `iterators`: döngü değişkeninin sahip olduğu değerler (string); her
    /// yinelemenin sonunda bırakılırlar.
    void emitLoopBody(BlockStmt* body, llvm::ArrayRef<Owned> iterators = {});
    void emitParallelFor(ForRangeStmt* node, mlir::Value start, mlir::Value end, mlir::Value step);
};

#endif // MLIRGENERATOR_H
