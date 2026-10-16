#ifndef SEMA_H
#define SEMA_H

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>
#include "ASTNode.h"

// ---------------------------------------------------------------------------
// Sema — semantik analiz
//
// AST üzerinde tek bir geçişle şunları yapar:
//   * İsim çözümleme: her Identifier'ın yerel / global / fonksiyon olduğunu
//     belirler, tanımsız ve çift tanımlı isimleri raporlar.
//   * Tip denetimi: her ifadeye SemType yazar. Tipler uyuşmadığında literalin
//     tipini değiştirir ya da araya CastExpr ekler; kod üretimine gelen ağaçta
//     bir işlemin iki tarafı her zaman aynı tiptedir.
//   * Akış analizi: break/continue'nun döngü içinde olduğunu, değer döndüren
//     fonksiyonların her yolda return ettiğini denetler ve her if/döngü için
//     yapısal (scf) mı yoksa bloklu (cf) mu üretileceğini işaretler
//     (IfStmt::needsCFG, LoopStmt::needsCFG / hasContinue).
//
// Hata varsa kod üretimine geçilmez; bu yüzden MLIRBackend, ağacın geçerli
// olduğunu varsayabilir.
// ---------------------------------------------------------------------------
class Sema : public ASTVisitor {
public:
    bool hasErrors() const { return !errors.empty(); }
    const std::vector<std::string>& getErrors() const { return errors; }

    void visit(Program* node) override;
    void visit(Type* node) override;
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
    void visit(BlockStmt* node) override;
    void visit(ExprStmt* node) override;
    void visit(VariableDecl* node) override;
    void visit(ParamDecl* node) override;
    void visit(FunctionDecl* node) override;
    void visit(LambdaExpr* node) override;
    void visit(IfStmt* node) override;
    void visit(WhileStmt* node) override;
    void visit(ForRangeStmt* node) override;
    void visit(ForCollectionStmt* node) override;
    void visit(BreakStmt* node) override;
    void visit(ContinueStmt* node) override;
    void visit(ReturnStmt* node) override;
    void visit(AssignmentStmt* node) override;

private:
    using ExprPtr = std::unique_ptr<Expression>;

    struct Symbol {
        enum Kind { Variable, Iterator, Global, Function };
        SemType type;
        Kind kind;
        int funcDepth; // tanımlandığı fonksiyon/lambda derinliği (global: 0)
        bool readOnly = false; // matris parametresi: atanamaz, elemanı değiştirilemez
        bool isParam = false;
        int scopeDepth = 0;    // tanımlandığı kapsamın `scopes` içindeki sırası (declare yazar)
        bool isGpu = false;    // fonksiyon: @GPU ile tanımlı
        // Fonksiyon / global: tanımlandığı modül ve başka modüllerden
        // kullanılabilir mi (`public`).
        int module = 0;
        bool isPublic = false;
        bool isExtern = false; // fonksiyon: gövdesi C'de (extern func)
        int paramIndex = -1;   // parametre: fonksiyondaki sırası
    };

    // Açık olan her if ve döngü için bir çerçeve. break/continue/return
    // görüldükçe ilgili çerçeveler işaretlenir.
    struct FlowFrame {
        bool isLoop;
        bool sawBreak = false;
        bool sawContinue = false;
        bool sawReturn = false;
    };

    std::vector<std::unordered_map<std::string, Symbol>> scopes;
    std::vector<FlowFrame> flow;
    std::vector<SemType> returnTypes; // içinde bulunulan fonksiyon/lambdaların dönüş tipleri
    int funcDepth = 0;
    // Bağlamın, denetlenmekte olan ifadeden beklediği tip (bkz. checkExpecting).
    // İfadeyi denetleyen visit metodu takeHint ile alır ve sıfırlar.
    SemType hint;
    std::vector<std::string> errors;

    void error(const ASTNode* node, const std::string& msg);

    // --- kapsam ---
    bool declare(const std::string& name, const Symbol& sym, const ASTNode* at);
    Symbol* lookup(const std::string& name);   // kaynakta yazılan isim
    Symbol* lookup(const Identifier* ident);   // çözülmüş ya da çözülmemiş düğüm
    Symbol* findGlobal(const std::string& linkName);

    // -----------------------------------------------------------------------
    // Modüller
    //
    // Bütün modüllerin fonksiyon ve globalleri tek bir global kapsamda,
    // tanımın adıyla durur: kök modülde kaynaktaki ad ("topla"), diğerlerinde
    // modül önekiyle ("araclar$vektor$norm"; bkz. Module::prefix). Kaynakta
    // yazılan bir isim, içinde bulunulan modüle göre tanımın adına çevrilir:
    //
    //   x          yerel değişken değilse: `from` ile alınan isim ya da modülün
    //              kendi tanımı
    //   v.x        v, `import ... as v` ile verilen ad (ya da `import a.b`
    //              için "a.b"); x o modülün tanımı
    //
    // Bir modül yalnızca kendi tanımlarını ve import ettiklerini görür; başka
    // modülün tanımını ancak `public` ise kullanabilir.
    // Çözülen Identifier'ın adı tanımın adıyla değiştirilir; kod üretimi ve
    // fonksiyon etkilerinin takibi yalnızca bu adı kullanır.
    // -----------------------------------------------------------------------
    struct ModuleNames {
        std::unordered_map<std::string, int> aliases;           // v → modül sırası
        std::unordered_map<std::string, std::string> imported;  // yerel ad → tanımın adı
    };
    const Program* program = nullptr;
    std::vector<ModuleNames> moduleNames;  // modül sırasıyla
    int currentModule = 0;                 // denetlenmekte olan bildirimin modülü

    // Bir modülün üst seviye tanımı (fonksiyon, global ya da struct).
    struct TopLevel {
        int module;
        bool isPublic;
    };
    std::unordered_map<std::string, TopLevel> topLevel;  // tanımın adı → sahibi

    void collectTopLevel();
    void bindImports();
    std::string linkNameOf(const std::string& name) const; // çözülemezse boş
    std::string globalLinkName(const std::string& name);   // global struct alanları dahil
    void reportUndefined(const Identifier* ident);

    // -----------------------------------------------------------------------
    // Struct'lar (SemaStruct.cpp)
    //
    // Struct tipli bir değişken, kapsamda kendisiyle birlikte alanları için de
    // birer sembol taşır: `Nokta p` tanımı "p", "p.x" ve "p.y" sembollerini
    // ekler (iç içe struct'larda "c.merkez.x" gibi). Böylece `p.x` sıradan bir
    // değişken gibi çözülür ve değişkenler için geçerli bütün denetimler
    // (salt okunur matris parametresi, paralel döngü kuralları, lambda
    // yakalama yasağı) alanlar için de kendiliğinden işler.
    //
    //   * Alanlar sayı, bool, char, string, dizi, liste, matris ya da başka
    //     bir struct olabilir. Bir struct kendisini içeremez.
    //   * Değer `Nokta(1.0, 2.0)` ile oluşturulur: argümanlar tanım sırasıyla
    //     alanlardır (FunctionCallExpr::isConstructor).
    //   * Struct üzerindeki işlem, alanlarına tek tek uygulanan işlemdir:
    //     `q = p` her alanı kendi tipinin kuralıyla atar (matris kopyalanır,
    //     liste paylaşılır); fonksiyona vermek alanları tek tek vermektir.
    //   * Struct listesi (list<Nokta>): elemanlar değerdir. `l[i]` bir kopya
    //     verir; elemanın alanına `l[i].x = ...` ile yazılır (AssignmentStmt'in
    //     hedefi MemberExpr olur; taban bir liste elemanı olmalıdır). Listenin
    //     kendisi için alan sembolü tanımlanmaz: `l.x` yazılamaz.
    //   * Global struct değişkeni ve @GPU fonksiyonda struct henüz yok.
    // -----------------------------------------------------------------------
    struct StructInfo {
        enum State { Pending, Resolving, Done };
        StructDecl* decl = nullptr;
        State state = Pending;
        SemType type; // alanları çözülmüş tip (hatalıysa Error)
    };
    std::unordered_map<std::string, StructInfo> structs; // tanımın adı → struct

    SemType resolveNamedType(Type* t);
    SemType resolveStruct(StructInfo& info);
    void checkConstructor(FunctionCallExpr* node, const Identifier* ident, StructInfo& info);
    // Değişkeni ve (struct ise) alanlarını içinde bulunulan kapsama ekler.
    void declareVariable(const std::string& name, const Symbol& sym, const ASTNode* at);
    void declareFields(const std::string& name, const Symbol& owner);

    // --- tipler ---
    SemType resolveType(Type* t);        // void dahil
    SemType resolveValueType(Type* t);   // değişken/parametre tipi: void olamaz
    SemType resolveReturnType(Type* t);  // dizi döndürülemez
    static SemType commonType(const SemType& a, const SemType& b);
    static bool isLiteralLike(const Expression* e);
    static void retypeLiteral(Expression* e, const SemType& t);
    bool literalFits(const Expression* e, const SemType& t);

    SemType check(ExprPtr& e);
    SemType checkExpecting(ExprPtr& e, const SemType& expected);
    SemType takeHint();
    void checkMatrixLiteral(ListLiteral* node, const SemType& matrixType);
    bool checkMatrixLiteralLevel(ListLiteral* node, const SemType& matrixType, int depth,
                                 std::vector<size_t>& shape);
    SemType checkMatrixBinary(BinaryExpr* node);
    void coerce(ExprPtr& slot, const SemType& target);
    SemType unify(ExprPtr& l, ExprPtr& r);
    SemType unifyAll(const std::vector<ExprPtr*>& parts);
    void checkCondition(ExprPtr& cond, const char* what);
    void checkBuiltinCall(FunctionCallExpr* node, const std::string& name,
                          const SemType& expected);
    /// scan(rows): satır kaynağı std.db.Rows; sonuç tipi `expected` (S ya da list<S>).
    void checkScan(FunctionCallExpr* node, const SemType& expected);

    // --- bildirimler ---
    void declareFunction(FunctionDecl* fn);
    void checkExtern(FunctionDecl* fn, const std::vector<SemType>& params, const SemType& ret);
    void checkGlobal(VariableDecl* v);
    std::vector<SemType> resolveParams(const std::vector<std::unique_ptr<ParamDecl>>& params);
    void declareParams(const std::vector<std::unique_ptr<ParamDecl>>& params,
                       const std::vector<SemType>& types);

    // --- akış ---
    static bool alwaysReturns(const Statement* s);
    void visitLoopBody(LoopStmt* loop);

    // -----------------------------------------------------------------------
    // Paralel döngü güvenliği (SemaParallel.cpp)
    //
    // `parallel for` gövdesinin yinelemeleri aynı anda çalışır. Derleyici,
    // iki yinelemenin aynı veriye yazamayacağını şu kurallarla garanti eder:
    //
    //   1. Döngü dışında tanımlı bir değişkene bütün olarak atama yapılamaz.
    //   2. Dışarıdaki bir dizi/liste/matrisin elemanına yalnızca indekslerden
    //      biri döngü değişkeninin kendisiyse yazılabilir (a[i] = ...); böylece
    //      her yineleme farklı elemana yazar.
    //   3. Döngüde yazılan bir dizi, aynı döngüde yalnızca aynı konumda döngü
    //      değişkeniyle indekslenerek okunabilir (a[i-1] okunamaz).
    //   4. push / pop / free, break ve return kullanılamaz.
    //   5. Çağrılan fonksiyonlar paylaşılan veriyi değiştirmemelidir (globale,
    //      başvuruyla aldığı dizi/listeye yazmamalı). Bu, fonksiyonların
    //      etkileri toplanarak program sonunda denetlenir.
    //
    // Kurallar bazı güvenli programları da reddeder; kabul edilen her program
    // ise yarışsızdır.
    // -----------------------------------------------------------------------
    struct ParallelAccess {
        std::string name;
        const ASTNode* at;
        unsigned iteratorMask; // döngü değişkeninin indeks olarak durduğu konumlar (bit i = i. indeks)
    };
    struct ParallelLoop {
        std::string iterator;
        int bodyDepth; // döngü değişkeninin kapsam sırası; daha küçük derinlik = döngü dışı
        std::vector<ParallelAccess> writes, reads;
    };
    // Bir fonksiyonun (ya da lambdanın) etkileri: paralel döngüden ve
    // görevden çağrılmaya uygun olup olmadığı. "...Reason" alanları boşsa o
    // etki yoktur; doluysa sebebini (hata mesajı için) söyler. Program
    // sonunda çağrı zinciri boyunca yayılırlar.
    struct FunctionEffects {
        std::string unsafeReason;          // paylaşılan veriyi değiştiriyor (paralel döngü kuralları)
        std::vector<std::string> callees;  // doğrudan çağırdığı fonksiyonlar
        // Boş değilse fonksiyon GPU'ya iş verir (bkz. GPU kuralları, kural 5);
        // paralel döngünün içinden çağrılamaz.
        std::string gpuWorkReason;
        // Görevler (bkz. SemaTask.cpp):
        std::string globalWriteReason;     // global değişkene yazıyor
        std::string awaitReason;           // await ile bekliyor
        std::string parallelReason;        // CPU'da paralel döngü çalıştırıyor (o da bekler)
        struct IndirectCall {
            SemType type;       // çağrılan fonksiyon değerinin tipi
            const ASTNode* at;
        };
        std::vector<IndirectCall> indirectCalls;  // fonksiyon değeri üzerinden çağrılar
    };
    struct ParallelCall {
        std::string callee;
        const ASTNode* at;
    };

    std::vector<ParallelLoop> parallelLoops;                  // içinde bulunulan paralel döngüler
    // fonksiyon adı → etkileri. Lambdalar da burada, lambdaKey adıyla durur.
    std::unordered_map<std::string, FunctionEffects> effects;
    FunctionEffects* currentEffects = nullptr;                // denetlenmekte olan fonksiyonunki
    std::vector<ParallelCall> parallelCalls;                  // paralel döngülerden yapılan çağrılar
    bool skipWholeRead = false;    // sıradaki Identifier bir indekslemenin/atamanın hedefi
    bool accessIsWrite = false;    // sıradaki ArrayAccessExpr bir atamanın hedefi

    bool inParallel() const { return !parallelLoops.empty(); }
    static bool isOutside(const Symbol& sym, const ParallelLoop& loop);
    unsigned iteratorMask(const ArrayAccessExpr* access, const ParallelLoop& loop);
    void noteUnsafe(const std::string& reason);
    void noteWholeRead(Identifier* ident, const Symbol& sym);
    void noteWholeWrite(Identifier* ident, const Symbol& sym);
    void noteElementAccess(ArrayAccessExpr* access, bool isWrite);
    void noteNestedWrite(Expression* target);
    void noteMutatingBuiltin(const ASTNode* at, const std::string& name);
    void noteCall(FunctionCallExpr* call);
    void finishParallelLoop(ParallelLoop& loop);
    void checkParallelCalls();

    // -----------------------------------------------------------------------
    // Görevler: marl, task<T>, await (SemaTask.cpp)
    //
    // `marl f(a, b)` f'yi iş parçacığı havuzunda yeni bir görev olarak
    // başlatır ve task<T> döner; `await(t)` görevin bitmesini bekleyip sonucu
    // verir. Kurallar:
    //
    //   1. Argümanlar görev başlamadan KOPYALANIR (liste ve dizi dahil);
    //      görev ile çağıran aynı listeyi asla görmez.
    //   2. Görev olarak çalışan fonksiyon ve çağırdıkları global değişkene
    //      yazamaz. Sonuç yalnızca await ile alınır.
    //   3. Görevin içinden fonksiyon değeriyle (dolaylı) çağrılabilecek her
    //      fonksiyon da kural 2'ye uymalı ve paralel döngü içermemelidir.
    //   4. await içeren (bekleyebilen) bir fonksiyon değer olarak kullanılamaz.
    //   5. paralel döngüde marl ve await kullanılamaz.
    //
    // RENKLENDİRME
    //
    // İş parçacığı havuzu sabit boyutludur. Havuzdaki bir iş parçacığı başka
    // bir görevi BLOKLAYARAK beklerse, bekleyenler havuzu doldurup beklenenleri
    // kuyrukta bırakabilir: program kilitlenir. Bu yüzden bir görevden
    // ulaşılabilen ve bekleyebilen (await ya da CPU paralel döngüsü içeren)
    // fonksiyonlar coroutine olarak üretilir (FunctionDecl::isAsync →
    // async.func): beklerken iş parçacığını bırakır, beklenen bitince devam
    // eder. Kullanıcı bunu görmez; görevlerden ulaşılmayan fonksiyonlar (ve
    // main) olduğu gibi kalır, onlarda await yalnızca kendi iş parçacığını
    // bekletir.
    // -----------------------------------------------------------------------
    struct TaskStart {
        std::string callee;   // doğrudan: fonksiyonun adı; dolaylıysa boş
        SemType valueType;    // dolaylı: çağrılan fonksiyon değerinin tipi
        const ASTNode* at;
        std::string owner;    // marl'ın yazıldığı fonksiyon (ya da lambdaKey)
    };
    // Değer olarak kullanılan bir fonksiyon ya da lambda: dolaylı çağrıların
    // hangi fonksiyonlara gidebileceği bunlardan (tipi aynı olanlar) çıkarılır.
    struct ValueFunction {
        std::string name;     // fonksiyonun adı ya da lambdaKey
        SemType type;
        const ASTNode* at;
        bool isLambda;
        std::string owner;    // değer olarak kullanıldığı fonksiyon
    };
    // Denetlenmekte olan fonksiyonun adı (lambdada lambdaKey).
    std::string currentFunction;
    std::vector<TaskStart> taskStarts;
    std::vector<ValueFunction> valueFunctions;
    std::unordered_map<std::string, FunctionDecl*> functionDecls; // ad → bildirim
    FunctionCallExpr* startingTask = nullptr; // denetlenmekte olan marl çağrısı

    static std::string lambdaKey(const LambdaExpr* lambda);
    static bool taskResultAllowed(const SemType& type);
    static bool taskArgumentAllowed(const SemType& type);
    void noteGlobalWrite(const std::string& name);
    void noteValueFunction(const std::string& name, const SemType& type, const ASTNode* at,
                           bool isLambda);
    void checkAwait(FunctionCallExpr* node);
    void checkTasks();

    // -----------------------------------------------------------------------
    // Liste literallerinin bırakılması (SemaBorrow.cpp)
    //
    // Listeler elle bırakılır; ama fonksiyona doğrudan verilen bir literalin
    // (f([1, 2, 3])) adı yoktur, kimse free edemez. Çağrılan fonksiyon o
    // parametreyi yalnızca ÖDÜNÇ alıyorsa literal deyim bitince bırakılır
    // (ListLiteral::freeAfterCall). Ödünç almak: parametre yalnızca
    // indekslenir, üzerinde dönülür, uzunluğu sorulur ya da yine ödünç alan
    // bir parametreye verilir; saklanmaz, döndürülmez, free edilmez.
    // Kanıtlanamazsa (dolaylı çağrı, parametrenin başka kullanımı) literal
    // bırakılmaz.
    // -----------------------------------------------------------------------
    struct ListParamUse {
        bool escapes = false;                              // ödünç almanın dışında kullanıldı
        std::vector<std::pair<std::string, int>> passes;  // (fonksiyon, parametre) olarak verildi
    };
    std::unordered_map<std::string, std::unordered_map<int, ListParamUse>> listParamUses;
    struct LiteralArgument {
        ListLiteral* literal;
        std::string callee;
        int index;
    };
    std::vector<LiteralArgument> literalArguments;
    std::pair<std::string, int> argumentContext{"", -1}; // sıradaki Identifier bir çağrının argümanı
    bool identIsIterated = false;                        // sıradaki Identifier for ... in'in koleksiyonu

    void noteListParamUse(const Identifier* ident, const Symbol& sym, bool borrowed);
    void markTemporaryListArguments();

    // -----------------------------------------------------------------------
    // @GPU fonksiyonların kuralları (SemaGpu.cpp)
    //
    // @GPU fonksiyonun matrisleri GPU belleğinde yaşar. İçindeki `parallel for`
    // döngüleri ve matris işlemleri GPU'da çalışır (birer "kernel" olur);
    // aradaki sıralı kod CPU'da çalışır ve yalnızca GPU'ya sırayla iş verir.
    // Bundan şu kurallar çıkar:
    //
    //   1. Tipler: yalnızca sayısal / bool / char değerler ve matrisler.
    //      (Dizi, liste, string ve fonksiyon değerleri CPU belleğinde yaşar.)
    //   2. Global değişken, print ve lambda kullanılamaz.
    //   3. Yalnızca @GPU fonksiyonlar çağrılabilir.
    //   4. Matris elemanına (m[i, j]) yalnızca `parallel for` içinde erişilir:
    //      döngünün dışındaki kod CPU'da çalışır, veri ise GPU'dadır.
    //   5. `parallel for` gövdesi GPU'da çalışır; oradan GPU'ya yeni iş
    //      verilemez. Bu yüzden gövdede matris işlemi (a + b, zeros, sum ...),
    //      iç içe `parallel for` ve bunları yapan fonksiyonlara çağrı olamaz.
    //      Gövdeden çağrılan fonksiyonlar yalnızca sayılarla çalışmalıdır.
    //
    // @CPU tarafında tek kısıt: @GPU fonksiyon yalnızca adıyla çağrılabilir
    // (değişkene konamaz); çağrı sırasında matrisler GPU'ya kopyalanır.
    // -----------------------------------------------------------------------
    bool gpuFunction = false;    // denetlenmekte olan fonksiyon @GPU
    bool identIsCallee = false;  // sıradaki Identifier bir çağrının hedefi

    bool inGpuKernel() const { return gpuFunction && inParallel(); }
    static bool gpuTypeAllowed(const SemType& type);
    void resolveTarget(FunctionDecl* fn);
    void checkGpuType(const SemType& type, const ASTNode* at);
    void noteGpuWork(const std::string& reason);
    void checkGpuExpr(Expression* expr);
    void checkGpuIdentifier(Identifier* ident, const Symbol& sym, bool isCallee);
    void checkGpuElementAccess(ArrayAccessExpr* access);
    void checkGpuBuiltin(FunctionCallExpr* call, const std::string& name);
    void checkGpuCall(FunctionCallExpr* call);
    void checkGpuParallelFor(ForRangeStmt* loop);
};

#endif //SEMA_H
