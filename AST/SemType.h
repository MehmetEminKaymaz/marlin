#ifndef SEMTYPE_H
#define SEMTYPE_H

#include <cstdint>
#include <string>
#include <vector>

// ---------------------------------------------------------------------------
// SemType
//
// Semantik analizin (Sema) her ifadeye yazdığı çözülmüş tip. AST'deki `Type`
// düğümü kaynak koddaki yazımı temsil eder; SemType ise anlamını: işaret
// bilgisi (int / uint), genişlik, dizi boyutu ve fonksiyon imzası burada
// tutulur. Kod üretimi (MLIRBackend) yalnızca SemType'a bakar.
// ---------------------------------------------------------------------------
struct SemType {
    enum class Kind { Error, Void, Bool, Char, Int, Float, String, Array, List, Matrix, Function };

    Kind kind = Kind::Error;
    unsigned width = 0;        // Int / Float
    bool isSigned = true;      // Int
    int64_t arraySize = 0;     // Array
    int rank = 0;              // Matrix: boyut sayısı (1 = vektör, 2 = matris, ...)
    std::vector<SemType> sub;  // Array / List / Matrix: {eleman};  Function: {dönüş, param0, param1, ...}

    static SemType Error()  { return {}; }
    static SemType Void()   { return make(Kind::Void); }
    static SemType Bool()   { return make(Kind::Bool, 1); }
    static SemType Char()   { return make(Kind::Char, 8); }
    static SemType String() { return make(Kind::String); }
    static SemType Int(unsigned width, bool isSigned = true) {
        SemType t = make(Kind::Int, width);
        t.isSigned = isSigned;
        return t;
    }
    static SemType Float(unsigned width) { return make(Kind::Float, width); }
    static SemType Array(const SemType& elem, int64_t size) {
        SemType t = make(Kind::Array);
        t.arraySize = size;
        t.sub.push_back(elem);
        return t;
    }
    // Büyüyebilen, yığın belleğinde yaşayan liste. Başvuru olarak dolaşır:
    // atama ve parametre geçişi kopyalamaz, aynı listeyi gösterir.
    static SemType List(const SemType& elem) {
        SemType t = make(Kind::List);
        t.sub.push_back(elem);
        return t;
    }
    // Çok boyutlu sayı tablosu. Boyut SAYISI (rank) tipin parçasıdır ve
    // derlerken bellidir; her boyutun uzunluğu çalışırken belli olur.
    // DEĞER olarak davranır: atama kopyadır, iki matris değişkeni asla aynı
    // belleği paylaşmaz. (Liste ise başvurudur.)
    static SemType Matrix(const SemType& elem, int rank) {
        SemType t = make(Kind::Matrix);
        t.rank = rank;
        t.sub.push_back(elem);
        return t;
    }
    static SemType Function(const SemType& ret, const std::vector<SemType>& params) {
        SemType t = make(Kind::Function);
        t.sub.push_back(ret);
        t.sub.insert(t.sub.end(), params.begin(), params.end());
        return t;
    }

    bool isError() const    { return kind == Kind::Error; }
    bool isVoid() const     { return kind == Kind::Void; }
    bool isBool() const     { return kind == Kind::Bool; }
    bool isChar() const     { return kind == Kind::Char; }
    bool isInt() const      { return kind == Kind::Int; }
    bool isFloat() const    { return kind == Kind::Float; }
    bool isString() const   { return kind == Kind::String; }
    bool isArray() const    { return kind == Kind::Array; }
    bool isList() const     { return kind == Kind::List; }
    bool isMatrix() const   { return kind == Kind::Matrix; }
    bool isFunction() const { return kind == Kind::Function; }
    bool isNumeric() const  { return isInt() || isFloat(); }
    // Tek bir SSA değeri / memref elemanı olarak saklanabilen tipler
    bool isScalar() const   { return isNumeric() || isBool() || isChar(); }

    const SemType& elementType() const { return sub.front(); }
    const SemType& returnType() const  { return sub.front(); }
    size_t paramCount() const          { return sub.size() - 1; }
    const SemType& paramType(size_t i) const { return sub[i + 1]; }

    bool operator==(const SemType& o) const {
        return kind == o.kind && width == o.width && arraySize == o.arraySize && rank == o.rank &&
               (kind != Kind::Int || isSigned == o.isSigned) && sub == o.sub;
    }
    bool operator!=(const SemType& o) const { return !(*this == o); }

    std::string str() const {
        switch (kind) {
            case Kind::Error:  return "<hata>";
            case Kind::Void:   return "void";
            case Kind::Bool:   return "bool";
            case Kind::Char:   return "char";
            case Kind::String: return "string";
            case Kind::Int: {
                std::string s = isSigned ? "int" : "uint";
                return width == 32 ? s : s + std::to_string(width);
            }
            case Kind::Float:  return width == 32 ? "float" : "float64";
            case Kind::Array:
                return elementType().str() + "[" + std::to_string(arraySize) + "]";
            case Kind::List:
                return "list<" + elementType().str() + ">";
            case Kind::Matrix:
                return "matrix<" + elementType().str() +
                       (rank == 2 ? "" : ", " + std::to_string(rank)) + ">";
            case Kind::Function: {
                std::string s = "(";
                for (size_t i = 0; i < paramCount(); ++i)
                    s += (i ? ", " : "") + paramType(i).str();
                return s + ") -> " + returnType().str();
            }
        }
        return "?";
    }

private:
    static SemType make(Kind k, unsigned w = 0) {
        SemType t;
        t.kind = k;
        t.width = w;
        return t;
    }
};

#endif //SEMTYPE_H
