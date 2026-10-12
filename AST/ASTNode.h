//
// Created by Eminf on 18.07.2026.
//

#ifndef ASTNODE_H
#define ASTNODE_H

#include <cstdint>
#include <memory>
#include <string>
#include <vector>
#include "ASTVisitor.h"
#include "Builtins.h"
#include "SemType.h"
#include "Enums/TypeKind.h"


class ASTNode {
    public:
    virtual ~ASTNode() = default;
    virtual void accept(ASTVisitor& visitor) = 0;

    int line = 0;
    int column = 0;
    // Düğümün yazıldığı modül (Program::modules içindeki sıra); 0 = derlenen dosya.
    int module = 0;

};

class Expression : public ASTNode {
public:
    // Sema tarafından doldurulur; kod üretimi tipleri buradan okur.
    SemType type;
};

class Statement : public ASTNode {
};

class Type : public ASTNode {
    public:
    TypeKind kind;
    int arraySize = 0;
    // Matrix: boyut sayısı. matrix<T> yazımında 2; geçersiz yazımda (tamsayı
    // olmayan sayı) -1 olur ve Sema raporlar.
    int rank = 0;
    std::string name;   // Named: kaynakta yazılan tip adı ("Nokta", "geometri.Nokta")
    std::unique_ptr<Type> elementType;
    std::vector<std::unique_ptr<Type>> paramTypes;
    std::unique_ptr<Type> returnType;

    Type(TypeKind kind) : kind(kind) {}
    void accept(ASTVisitor& visitor) override {visitor.visit(this);}
};

// import a.b;   import a.b as v;   from a.b import f, g as h;
//
// Ziyaretçiler bu düğümü gezmez: ModuleLoader dosyayı bulup yükler, Sema
// isimleri bağlar.
class ImportDecl : public ASTNode {
public:
    struct Name {
        std::string name;   // modüldeki adı
        std::string alias;  // `as` ile verilen ad (yoksa boş)
    };
    std::string modulePath;   // "a.b"
    std::string alias;        // import ... as v (yoksa boş)
    bool isFrom = false;      // from ... import ...
    std::vector<Name> names;  // from ile alınan isimler
    int target = -1;          // Program::modules içindeki sıra (ModuleLoader doldurur)
    void accept(ASTVisitor&) override {}
};

// Bir kaynak dosya. Program, derlenen dosyadan (kök modül) ve onun doğrudan
// ya da dolaylı import ettiği dosyalardan oluşur.
struct Module {
    std::string name;    // import yolu ("araclar.vektor"); kök modülde boş
    std::string path;    // kaynak dosyanın yolu (hata mesajları için)
    // Kök dışındaki modüllerin fonksiyon ve globalleri bu önekle adlandırılır
    // ("araclar$vektor$norm"); böylece farklı modüllerdeki aynı isimler
    // çakışmaz. '$' kaynak kodda isim içinde yazılamaz. Kök modülde boştur:
    // `main` ve diğer isimler olduğu gibi kalır.
    std::string prefix;
    std::vector<std::unique_ptr<ImportDecl>> imports;
};

class Program : public ASTNode {
    public:
    std::vector<Module> modules;   // [0] kök modül
    // Bütün modüllerin bildirimleri (her birinin `module` alanı sahibini gösterir).
    std::vector<std::unique_ptr<Statement>> declarations;
    void accept(ASTVisitor& visitor) override {visitor.visit(this);};
};

class IntegerLiteral : public Expression {
public:
    int64_t value;
    explicit IntegerLiteral(int64_t v) : value(v) {}
    void accept(ASTVisitor& visitor) override { visitor.visit(this); }
};

class FloatLiteral : public Expression {
public:
    double value;
    explicit FloatLiteral(double v) : value(v) {}
    void accept(ASTVisitor& visitor) override { visitor.visit(this); }
};

class BooleanLiteral : public Expression {
public:
    bool value;
    explicit BooleanLiteral(bool v) : value(v) {}
    void accept(ASTVisitor& visitor) override { visitor.visit(this); }
};

class CharLiteral : public Expression {
public:
    char value;
    explicit CharLiteral(char v) : value(v) {}
    void accept(ASTVisitor& visitor) override { visitor.visit(this); }
};

class StringLiteral : public Expression {
public:
    std::string value;
    explicit StringLiteral(const std::string& v) : value(v) {}
    void accept(ASTVisitor& visitor) override { visitor.visit(this); }
};

class Identifier : public Expression {
public:
    // İsmin neye çözüldüğü (Sema doldurur)
    enum class Ref { Unresolved, Local, Global, Function };

    // Kaynakta yazılan isim ("x", "v.norm"). Global ya da fonksiyona
    // çözüldüğünde Sema bunu tanımın adıyla değiştirir (bkz. Module::prefix).
    std::string name;
    Ref ref = Ref::Unresolved;
    explicit Identifier(const std::string& n) : name(n) {}
    void accept(ASTVisitor& visitor) override { visitor.visit(this); }
};

class ListLiteral : public Expression {
public:
    std::vector<std::unique_ptr<Expression>> elements;
    void accept(ASTVisitor& visitor) override { visitor.visit(this); }
};

class ArrayAccessExpr : public Expression {
public:
    std::unique_ptr<Expression> target;
    // Dizi ve listede tek indeks (a[i]), matriste iki indeks (m[i, j]).
    std::vector<std::unique_ptr<Expression>> indices;
    explicit ArrayAccessExpr(std::unique_ptr<Expression> t) : target(std::move(t)) {}
    void accept(ASTVisitor& visitor) override { visitor.visit(this); }
};

class FunctionCallExpr : public Expression {
public:
    std::unique_ptr<Expression> target;
    std::vector<std::unique_ptr<Expression>> arguments;
    // Çağrı bir yerleşik fonksiyona (print, sqrt ...) aitse Sema işaretler;
    // bu durumda `target` çözülmez ve kod üretiminde kullanılmaz.
    Builtin builtin = Builtin::None;
    // Çağrı bir struct oluşturuyorsa (Nokta(1.0, 2.0)) Sema işaretler:
    // argümanlar tanım sırasıyla alanların değerleridir, `type` struct'ın
    // tipidir ve `target` çözülmez.
    bool isConstructor = false;
    FunctionCallExpr(std::unique_ptr<Expression> t) : target(std::move(t)) {}
    void accept(ASTVisitor& visitor) override { visitor.visit(this); }
};

// Bir ifadenin alanı: f().x, (a).y
//
// Değişkenin alanı (p.x) bu düğümle değil, noktalı adlı bir Identifier ile
// gösterilir (bkz. qualifiedName); bu düğüm, struct değeri bir değişken
// olmayan ifadeler içindir.
class MemberExpr : public Expression {
public:
    std::unique_ptr<Expression> base;
    std::string field;
    int fieldIndex = -1; // alanın struct içindeki sırası (Sema doldurur)
    MemberExpr(std::unique_ptr<Expression> b, std::string f)
        : base(std::move(b)), field(std::move(f)) {}
    void accept(ASTVisitor& visitor) override { visitor.visit(this); }
};

class UnaryExpr : public Expression {
public:
    enum Op { Negate, Not };
    Op op;
    std::unique_ptr<Expression> operand;
    UnaryExpr(Op o, std::unique_ptr<Expression> e) : op(o), operand(std::move(e)) {}
    void accept(ASTVisitor& visitor) override { visitor.visit(this); }
};

class BinaryExpr : public Expression {
public:
    enum Op { Add, Sub, Mul, Div, Mod, Eq, Neq, Lt, Lte, Gt, Gte, And, Or };
    Op op;
    std::unique_ptr<Expression> left;
    std::unique_ptr<Expression> right;
    BinaryExpr(Op o, std::unique_ptr<Expression> l, std::unique_ptr<Expression> r)
        : op(o), left(std::move(l)), right(std::move(r)) {}
    void accept(ASTVisitor& visitor) override { visitor.visit(this); }
};

// Örtük tip dönüşümü. Kaynak kodda karşılığı yoktur; Sema, tiplerin uyuşmadığı
// yerlere ekler (hedef tip `type` alanındadır).
class CastExpr : public Expression {
public:
    std::unique_ptr<Expression> operand;
    explicit CastExpr(std::unique_ptr<Expression> e) : operand(std::move(e)) {}
    void accept(ASTVisitor& visitor) override { visitor.visit(this); }
};

class BlockStmt : public Statement {
public:
    std::vector<std::unique_ptr<Statement>> statements;
    void accept(ASTVisitor& visitor) override { visitor.visit(this); }
};

class ExprStmt : public Statement {
public:
    std::unique_ptr<Expression> expr;
    ExprStmt(std::unique_ptr<Expression> e) : expr(std::move(e)) {}
    void accept(ASTVisitor& visitor) override { visitor.visit(this); }
};

class VariableDecl : public Statement {
public:
    std::string name;
    std::unique_ptr<Type> type;
    std::unique_ptr<Expression> initializer;
    bool isLet;
    // Global değişken: başka modüller import edebilir (`public` ile tanımlı).
    bool isPublic = false;
    SemType varType; // Sema doldurur (let için çıkarılan tip dahil)
    VariableDecl(std::string n, std::unique_ptr<Type> t, std::unique_ptr<Expression> init, bool let = false)
        : name(std::move(n)), type(std::move(t)), initializer(std::move(init)), isLet(let) {}
    void accept(ASTVisitor& visitor) override { visitor.visit(this); }
};

class AssignmentStmt : public Statement {
public:
    std::unique_ptr<Expression> target;
    std::unique_ptr<Expression> value;

    AssignmentStmt(std::unique_ptr<Expression> t, std::unique_ptr<Expression> v)
        : target(std::move(t)), value(std::move(v)) {}

    void accept(ASTVisitor& visitor) override { visitor.visit(this); }
};

class ParamDecl : public ASTNode {
public:
    std::string name;
    std::unique_ptr<Type> type;
    ParamDecl(std::string n, std::unique_ptr<Type> t) : name(std::move(n)), type(std::move(t)) {}
    void accept(ASTVisitor& visitor) override { visitor.visit(this); }
};

class FunctionDecl : public Statement {
public:
    std::string name;
    std::vector<std::unique_ptr<ParamDecl>> params;
    std::unique_ptr<Type> returnType;
    std::unique_ptr<BlockStmt> body;
    SemType fnType; // Sema doldurur

    // Başka modüller import edebilir (`public` ile tanımlı). Yazılmadıysa
    // fonksiyon private'tır: yalnızca kendi modülünde kullanılabilir.
    bool isPublic = false;
    // extern: gövdesi çalışma zamanı kütüphanesinde, C ile yazılı (`body`
    // boştur). Yalnızca standart kütüphane modüllerinde bulunur.
    bool isExtern = false;
    std::string externName;  // C'deki adı: std.file / c_exists → minik_file_exists
    bool isPure = false;     // extern: yan etkisi yok (@pure); Sema doldurur
    // Fonksiyonun başına yazılan @CPU / @GPU (extern'de @pure); yazılmadıysa boş.
    std::string annotation;
    // Hedef (Sema doldurur). @GPU: fonksiyonun matrisleri GPU belleğinde
    // yaşar; içindeki `parallel for` ve matris işlemleri GPU'da çalışır.
    bool isGpu = false;
    // @GPU fonksiyonun parametre ya da dönüş tipinde matris var: CPU'dan
    // çağrılırken veriler GPU'ya (ve sonuç geri) kopyalanmalıdır.
    bool hasMatrixSignature() const {
        if (!fnType.isFunction()) return false;
        if (fnType.returnType().isMatrix()) return true;
        for (size_t i = 0; i < fnType.paramCount(); ++i)
            if (fnType.paramType(i).isMatrix()) return true;
        return false;
    }
    void accept(ASTVisitor& visitor) override { visitor.visit(this); }
};

// struct Nokta { float64 x; float64 y; }
//
// Bir tip tanımıdır, çalıştırılan bir şey değil; ziyaretçiler gezmez. Sema
// tipini çözer (alanlar SemType'ta taşınır), kod üretimi yalnızca o tipe bakar.
class StructDecl : public Statement {
public:
    struct Field {
        std::string name;
        std::unique_ptr<Type> type;
    };
    std::string name;
    std::vector<Field> fields;
    // Başka modüller kullanabilir (`public` ile tanımlı); yazılmadıysa private.
    bool isPublic = false;
    void accept(ASTVisitor&) override {}
};

class LambdaExpr : public Expression {
public:
    std::vector<std::unique_ptr<ParamDecl>> params;
    std::unique_ptr<Type> returnType;
    // Gövde ya tek bir ifadedir ( | expr ) ya da bir bloktur; biri doludur.
    std::unique_ptr<Expression> exprBody;
    std::unique_ptr<BlockStmt> blockBody;
    void accept(ASTVisitor& visitor) override { visitor.visit(this); }
};

class IfStmt : public Statement {
public:
    struct ElseIf {
        std::unique_ptr<Expression> condition;
        std::unique_ptr<BlockStmt> block;
    };
    std::unique_ptr<Expression> condition;
    std::unique_ptr<BlockStmt> trueBlock;
    std::vector<ElseIf> elseIfs;
    std::unique_ptr<BlockStmt> elseBlock;
    // Dallardan biri dışarı atlıyor mu (return ya da dıştaki döngüye
    // break/continue)? Öyleyse scf.if kullanılamaz, cf blokları üretilir.
    bool needsCFG = false;
    void accept(ASTVisitor& visitor) override { visitor.visit(this); }
};

// Döngülerin ortak kısmı. Akış bilgilerini Sema doldurur.
class LoopStmt : public Statement {
public:
    std::unique_ptr<BlockStmt> body;
    // Gövdede bu döngüye ait bir break ya da herhangi bir return var:
    // döngü yapısal (scf) olarak üretilemez, cf bloklarına düşülür.
    bool needsCFG = false;
    // Gövdede bu döngüye ait bir continue var.
    bool hasContinue = false;
    // `parallel for`: yinelemeler aynı anda çalışabilir. Sema, gövdenin buna
    // uygun olduğunu (yinelemelerin birbirinin verisine dokunmadığını) denetler.
    bool isParallel = false;
};

class WhileStmt : public LoopStmt {
public:
    std::unique_ptr<Expression> condition;
    void accept(ASTVisitor& visitor) override { visitor.visit(this); }
};

class ForRangeStmt : public LoopStmt {
public:
    std::string iteratorName;
    std::unique_ptr<Expression> start;
    std::unique_ptr<Expression> end;
    std::unique_ptr<Expression> step;
    SemType iterType; // Sema doldurur
    void accept(ASTVisitor& visitor) override { visitor.visit(this); }
};

class ForCollectionStmt : public LoopStmt {
public:
    std::string iteratorName;
    std::unique_ptr<Expression> collection;
    SemType iterType; // Sema doldurur (koleksiyonun eleman tipi)
    void accept(ASTVisitor& visitor) override { visitor.visit(this); }
};

class BreakStmt : public Statement {
public:
    void accept(ASTVisitor& visitor) override { visitor.visit(this); }
};

class ContinueStmt : public Statement {
public:
    void accept(ASTVisitor& visitor) override { visitor.visit(this); }
};

class ReturnStmt : public Statement {
public:
    std::unique_ptr<Expression> expr;
    void accept(ASTVisitor& visitor) override { visitor.visit(this); }
};





#endif //ASTNODE_H
