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
    enum class Kind { Error, Void, Bool, Char, Int, Float, String, Array, List, Matrix, Function, Struct, Task, Map };

    Kind kind = Kind::Error;
    unsigned width = 0;        // Int / Float
    bool isSigned = true;      // Int
    int64_t arraySize = 0;     // Array
    int rank = 0;              // Matrix: boyut sayısı (1 = vektör, 2 = matris, ...)
    // Array / List / Matrix: {eleman};  Task: {sonuç};  Function: {dönüş, param0, param1, ...};
    // Struct: alanların tipleri, tanım sırasıyla;  Map: {anahtar, değer}
    std::vector<SemType> sub;
    std::string name;                     // Struct: tanımın adı (modül önekiyle)
    std::vector<std::string> fieldNames;  // Struct: alan adları (`sub` ile aynı sırada)

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

    // marl ile başlatılan görev: sonucu (henüz) hazır olmayabilir; `await`
    // bekler ve sonucu verir. Başvuru olarak dolaşır (string gibi başvuru
    // sayılır): kopyalanınca iki değişken aynı görevi gösterir.
    static SemType Task(const SemType& result) {
        SemType t = make(Kind::Task);
        t.sub.push_back(result);
        return t;
    }

    // Anahtardan değere eşleme. Anahtarlar eklenme sırasını korur. Liste gibi
    // başvuru olarak dolaşır ve elle bırakılır (free). Tek bir değer olarak
    // saklanmaz; yapraklarına ayrılır (bkz. leavesOf, MAP).
    static SemType Map(const SemType& key, const SemType& value) {
        SemType t = make(Kind::Map);
        t.sub.push_back(key);
        t.sub.push_back(value);
        return t;
    }

    // Kullanıcının tanımladığı kayıt tipi. İki struct tipi adları aynıysa
    // aynıdır. Struct değeri tek bir değer olarak saklanmaz; alanlarına
    // ayrılır (bkz. leavesOf).
    static SemType Struct(const std::string& name, const std::vector<std::string>& fieldNames,
                          const std::vector<SemType>& fieldTypes) {
        SemType t = make(Kind::Struct);
        t.name = name;
        t.fieldNames = fieldNames;
        t.sub = fieldTypes;
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
    bool isStruct() const   { return kind == Kind::Struct; }
    bool isTask() const     { return kind == Kind::Task; }
    bool isMap() const      { return kind == Kind::Map; }
    bool isNumeric() const  { return isInt() || isFloat(); }
    // Tek bir SSA değeri / memref elemanı olarak saklanabilen tipler
    bool isScalar() const   { return isNumeric() || isBool() || isChar(); }

    const SemType& elementType() const { return sub.front(); }
    const SemType& returnType() const  { return sub.front(); }
    size_t paramCount() const          { return sub.size() - 1; }
    const SemType& paramType(size_t i) const { return sub[i + 1]; }
    const SemType& keyType() const     { return sub[0]; }  // Map
    const SemType& valueType() const   { return sub[1]; }  // Map

    // Tek bir değer olarak saklanmayıp yapraklarına ayrılan tipler: struct,
    // map ve elemanı böyle olan liste (list<Nokta>; bkz. leavesOf).
    bool isComposite() const {
        return isStruct() || isMap() || (isList() && elementType().isComposite());
    }
    // Tipin kendisi ya da (struct ise, iç içe) alanlarından biri map mi?
    bool containsMap() const {
        if (isMap()) return true;
        if (!isStruct()) return false;
        for (const SemType& field : sub)
            if (field.containsMap()) return true;
        return false;
    }

    // Struct: alanın sırası; yoksa -1.
    int fieldIndex(const std::string& field) const {
        for (size_t i = 0; i < fieldNames.size(); ++i)
            if (fieldNames[i] == field) return static_cast<int>(i);
        return -1;
    }
    // Tipin kendisi ya da (struct ise, iç içe) alanlarından biri matris mi?
    bool containsMatrix() const {
        if (isMatrix()) return true;
        if (!isStruct()) return false;
        for (const SemType& field : sub)
            if (field.containsMatrix()) return true;
        return false;
    }
    // Tipin kendisi ya da (struct ise, iç içe) alanlarından biri fonksiyon mu?
    // (Böyle bir değer başlangıç değeri olmadan tanımlanamaz: çağrılacak bir
    // fonksiyonu olmazdı.)
    bool containsFunction() const {
        if (isFunction()) return true;
        if (!isStruct()) return false;
        for (const SemType& field : sub)
            if (field.containsFunction()) return true;
        return false;
    }

    bool operator==(const SemType& o) const {
        return kind == o.kind && width == o.width && arraySize == o.arraySize && rank == o.rank &&
               (kind != Kind::Int || isSigned == o.isSigned) && name == o.name && sub == o.sub;
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
            case Kind::Struct: return name;
            case Kind::Task:   return "task<" + elementType().str() + ">";
            case Kind::Map:    return "map<" + keyType().str() + ", " + valueType().str() + ">";
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

// ---------------------------------------------------------------------------
// Yapraklar — struct'ın alanlarına ayrılması
//
// Struct tipli bir değer bellekte tek parça durmaz: her alanı kendi tipinin
// kurallarıyla, ayrı bir değer olarak yaşar. İç içe struct'lar da açılır;
// geriye kalan struct olmayan alanlara "yaprak" denir:
//
//   struct Nokta  { float64 x; float64 y; }
//   struct Cember { Nokta merkez; float64 r; list<int> etiketler; }
//
//   Cember c   →   c.merkez.x   float64
//                  c.merkez.y   float64
//                  c.r          float64
//                  c.etiketler  list<int>
//
// Struct üzerindeki her işlem (kopyalama, fonksiyona verme, döndürme) aynı
// işlemin yapraklara tek tek uygulanmasıdır. Yaprakların sırası her yerde
// aynıdır (tanım sırası); kod üretimi değerleri bu sırayla taşır.
//
// STRUCT LİSTESİ
//
// list<Nokta> de tek parça durmaz: elemanın her yaprağı için ayrı bir liste
// tutulur ("alan başına bir sütun"). Listenin yaprakları, elemanın
// yapraklarının listeleridir ve aynı sıradadır:
//
//   list<Cember> l   →   l.merkez.x   list<float64>
//                        l.merkez.y   list<float64>
//                        l.r          list<float64>
//                        l.etiketler  list<list<int>>
//
// Sütunların uzunlukları her zaman eşittir (eleman ekleme/çıkarma hepsine
// birden uygulanır). Sütun adları kaynakta yazılamaz; yalnızca kod üretiminin
// değişken tablosunda yaşar.
//
// MAP
//
// map<K, V> üç parçadan oluşan gizli bir struct gibi saklanır:
//
//   map<string, Nokta> m   →   m.#index      int64         anahtar → satır tablosu
//                              m.#keys       list<string>  anahtarlar, eklenme sırasıyla
//                              m.#values.x   list<float64> değerler: list<V>'nin
//                              m.#values.y   list<float64>   yaprakları
//
// Satır r'deki anahtar keys[r], değeri values[r]'dir. Tablo çalışma zamanının
// bir nesnesidir (runtime/marlin_map.c): anahtarı satır numarasına çevirir.
// Listeler gibi yaprakların hepsi başvurudur (tablonun adresi ve listelerin
// hücreleri değişmez), bu yüzden map'i kopyalamak aynı map'i paylaşmaktır.
// '#' kaynakta bir adda bulunamaz; yapraklar yalnızca derleyicinin tablosundadır.
// ---------------------------------------------------------------------------
struct SemLeaf {
    std::string path;  // "c.merkez.x": verilen kök ad + alan yolu
    SemType type;
};

// Map yapraklarının sırası (bkz. MAP): tablo, anahtarlar, sonra değerler.
constexpr const char* kMapIndexLeaf = ".#index";
constexpr size_t kMapIndexLeafPos = 0;
constexpr size_t kMapKeysLeafPos = 1;
constexpr size_t kMapValuesLeafPos = 2;

inline void collectLeaves(const SemType& type, const std::string& path, std::vector<SemLeaf>& out) {
    if (type.isStruct()) {
        for (size_t i = 0; i < type.sub.size(); ++i)
            collectLeaves(type.sub[i], path + "." + type.fieldNames[i], out);
        return;
    }
    if (type.isMap()) {
        out.push_back({path + kMapIndexLeaf, SemType::Int(64)});
        out.push_back({path + ".#keys", SemType::List(type.keyType())});
        collectLeaves(SemType::List(type.valueType()), path + ".#values", out);
        return;
    }
    if (type.isComposite()) { // struct listesi: elemanın her yaprağı için bir liste
        const size_t first = out.size();
        collectLeaves(type.elementType(), path, out);
        for (size_t i = first; i < out.size(); ++i) out[i].type = SemType::List(out[i].type);
        return;
    }
    out.push_back({path, type});
}

// Struct olmayan bir tipin tek yaprağı kendisidir; void'in yaprağı yoktur.
inline std::vector<SemLeaf> leavesOf(const SemType& type, const std::string& root = "") {
    std::vector<SemLeaf> leaves;
    if (!type.isVoid()) collectLeaves(type, root, leaves);
    return leaves;
}

#endif //SEMTYPE_H
