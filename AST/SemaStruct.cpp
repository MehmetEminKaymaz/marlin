// Sema — struct'lar (kurallar: Sema.h)

#include "Sema.h"

#include <unordered_set>

// Tip olarak yazılan isim: `Nokta`, `geometri.Nokta`.
SemType Sema::resolveNamedType(Type* t) {
    auto found = structs.find(linkNameOf(t->name));
    if (found == structs.end()) {
        error(t, "tanimsiz tip: '" + t->name + "'");
        return SemType::Error();
    }
    StructDecl* decl = found->second.decl;
    if (!decl->isPublic && decl->module != currentModule) {
        error(t, "'" + t->name + "' private; baska bir modulden kullanilamaz "
                 "(tanimina 'public' yazin)");
        return SemType::Error();
    }
    return resolveStruct(found->second);
}

// Struct'ın alan tiplerini çözer. Alan başka bir struct ise önce o çözülür;
// çözülmekte olan bir struct'a yeniden gelinirse struct kendisini içeriyordur
// (değer olarak saklandığı için sonsuz büyüklükte olurdu).
SemType Sema::resolveStruct(StructInfo& info) {
    if (info.state == StructInfo::Done) return info.type;
    StructDecl* decl = info.decl;
    if (info.state == StructInfo::Resolving) {
        error(decl, "struct '" + decl->name + "' kendisini iceriyor (dogrudan ya da baska bir "
                    "struct uzerinden)");
        return SemType::Error();
    }
    info.state = StructInfo::Resolving;

    // Alan tiplerindeki isimler struct'ın tanımlandığı modüle göre çözülür.
    const int savedModule = currentModule;
    currentModule = decl->module;

    std::vector<std::string> names;
    std::vector<SemType> types;
    std::unordered_set<std::string> seen;
    bool ok = !decl->fields.empty();
    if (decl->fields.empty()) error(decl, "struct en az bir alan icermeli");

    for (auto& field : decl->fields) {
        SemType type = resolveValueType(field.type.get());
        if (type.isTask()) {
            error(field.type.get(), "struct alani task olamaz (henuz); gorevleri bir listede tutun");
            type = SemType::Error();
        }
        if (!seen.insert(field.name).second) {
            error(field.type.get(), "'" + field.name + "' alani birden cok kez tanimlanmis");
            type = SemType::Error();
        }
        ok = ok && !type.isError();
        names.push_back(field.name);
        types.push_back(type);
    }

    currentModule = savedModule;
    info.type = ok ? SemType::Struct(decl->name, names, types) : SemType::Error();
    info.state = StructInfo::Done;
    return info.type;
}

// Nokta(1.0, 2.0) — argümanlar tanım sırasıyla alanların değerleridir ve
// fonksiyon argümanları gibi alan tipine getirilir.
void Sema::checkConstructor(FunctionCallExpr* node, const Identifier* ident, StructInfo& info) {
    StructDecl* decl = info.decl;
    const bool visible = decl->isPublic || decl->module == currentModule;
    if (!visible)
        error(ident, "'" + ident->name + "' private; baska bir modulden kullanilamaz "
                     "(tanimina 'public' yazin)");

    const SemType type = visible ? resolveStruct(info) : SemType::Error();
    const bool countOk = type.isStruct() && node->arguments.size() == type.sub.size();
    for (size_t i = 0; i < node->arguments.size(); ++i) {
        if (countOk) checkExpecting(node->arguments[i], type.sub[i]);
        else check(node->arguments[i]);
    }
    if (!type.isStruct()) return;
    if (!countOk) {
        error(node, "'" + type.str() + "' " + std::to_string(type.sub.size()) +
                    " alan bekler, verilen " + std::to_string(node->arguments.size()));
        return;
    }
    for (size_t i = 0; i < node->arguments.size(); ++i)
        coerce(node->arguments[i], type.sub[i]);

    node->isConstructor = true;
    node->type = type;
}

// f().x — struct değeri bir değişken olmayan ifadenin alanı.
void Sema::visit(MemberExpr* node) {
    node->type = SemType::Error();
    SemType base = check(node->base);
    if (base.isError()) return;
    if (!base.isStruct()) {
        error(node, "'." + node->field + "' ile alan erisimi bir struct bekler (bulunan: " +
                    base.str() + ")");
        return;
    }
    node->fieldIndex = base.fieldIndex(node->field);
    if (node->fieldIndex < 0) {
        error(node, "'" + base.str() + "' tipinde '" + node->field + "' adinda bir alan yok");
        return;
    }
    node->type = base.sub[node->fieldIndex];
}

// scan(rows) — sorgu sonucunun satırlarını struct'lara doldurur (bkz.
// std/db.marl). Doldurulacak tip bağlamdan gelir: list<S> bütün satırlar, S
// ilk satır. S'nin alanları sütunlara adlarıyla eşlenir; alanlar tek değerlik
// olmalıdır (sayı, bool, string).
void Sema::checkScan(FunctionCallExpr* node, const SemType& expected) {
    const SemType& source = node->arguments[0]->type;
    if (!source.isStruct() || source.name != "std$db$Rows") {
        error(node->arguments[0].get(), "'scan' bir satir kaynagi (std.db.Rows) bekler, ornegin "
                                        "scan(r.rows) (bulunan: " + source.str() + ")");
        return;
    }
    if (inParallel()) {
        error(node, "paralel dongude 'scan' kullanilamaz");
        return;
    }

    const SemType row = expected.isList() ? expected.elementType() : expected;
    if (!row.isStruct()) {
        error(node, "'scan' satirlari hangi struct'a dolduracagini bilmiyor; tipi belirtin "
                    "(or. list<Kullanici> l = scan(r.rows); ya da Kullanici k = scan(r.rows);)");
        return;
    }
    bool ok = true;
    for (size_t i = 0; i < row.sub.size(); ++i) {
        const SemType& field = row.sub[i];
        if (field.isInt() || field.isFloat() || field.isBool() || field.isString()) continue;
        error(node, "'scan' ile doldurulan " + row.str() + " struct'inin '" + row.fieldNames[i] +
                    "' alani " + field.str() + " olamaz (sayi, bool ya da string olmali)");
        ok = false;
    }
    if (!ok) return;

    // Satır kaynağının fonksiyonları dolaylı çağrılır (görev ve paralel döngü
    // kuralları için).
    noteUnsafe("degisken uzerinden (dolayli) fonksiyon cagiriyor");
    if (currentEffects)
        for (size_t i = 1; i < source.sub.size(); ++i)
            currentEffects->indirectCalls.push_back({source.sub[i], node});
    node->type = expected;
}

// to_json(x) / from_json(v) — bkz. std/json.marl, MLIRBackend/MLIRGeneratorJson.cpp.
//
// Yazılabilen (to_json): sayılar, bool, char, string, sabit diziler, struct,
// list<T>, map<K, V> (anahtar JSON'da string olarak yazılır) ve json.Value.
// Okunabilen (from_json): sayılar, bool, string, struct, list<T> ve
// map<string, V>. Matris, fonksiyon ve görev ikisinde de yok.
static const char* const kJsonValueType = "std$json$Value";

static bool jsonEncodable(const SemType& t, std::string& why) {
    if (t.isScalar() || t.isString()) return true;
    if (t.isStruct()) {
        if (t.name == kJsonValueType) return true;
        for (const SemType& field : t.sub)
            if (!jsonEncodable(field, why)) return false;
        return true;
    }
    if (t.isArray() || t.isList()) return jsonEncodable(t.elementType(), why);
    if (t.isMap()) return jsonEncodable(t.valueType(), why);
    why = t.str();
    return false;
}

static bool jsonDecodable(const SemType& t, std::string& why) {
    if (t.isNumeric() || t.isBool() || t.isString()) return true;
    if (t.isStruct() && t.name != kJsonValueType) {
        for (const SemType& field : t.sub)
            if (!jsonDecodable(field, why)) return false;
        return true;
    }
    if (t.isList()) return jsonDecodable(t.elementType(), why);
    if (t.isMap()) {
        if (!t.keyType().isString()) {
            why = t.str() + " (JSON nesnesinin anahtarlari string; map<string, ...> kullanin)";
            return false;
        }
        return jsonDecodable(t.valueType(), why);
    }
    why = t.str();
    return false;
}

void Sema::checkJson(FunctionCallExpr* node, const SemType& expected) {
    const SemType& arg = node->arguments[0]->type;
    std::string why;
    if (node->builtin == Builtin::ToJson) {
        if (!jsonEncodable(arg, why)) {
            error(node->arguments[0].get(), "'to_json' bu tipi JSON'a yazamaz: " + why +
                                            " (sayi, bool, char, string, dizi, struct, liste ve map yazilabilir)");
            return;
        }
        node->type = SemType::String();
        return;
    }

    if (!arg.isStruct() || arg.name != kJsonValueType) {
        error(node->arguments[0].get(), "'from_json' bir json.Value bekler, ornegin from_json(d.root) "
                                        "(bulunan: " + arg.str() + ")");
        return;
    }
    if (expected.isError() || expected.isVoid()) {
        error(node, "'from_json' hangi tipe dolduracagini bilmiyor; tipi belirtin "
                    "(or. Kullanici k = from_json(d.root);)");
        return;
    }
    if (!jsonDecodable(expected, why)) {
        error(node, "'from_json' bu tipi dolduramaz: " + why +
                    " (sayi, bool, string, struct, liste ve map<string, ...> doldurulabilir)");
        return;
    }
    // Hata belgeye kaydedilir (bkz. std/json.marl): paralel döngüde yinelemeler
    // aynı belgeye yazabilirdi.
    noteUnsafe("from_json ile bir JSON belgesine hata kaydediyor");
    if (inParallel()) {
        error(node, "paralel dongude 'from_json' kullanilamaz");
        return;
    }
    node->type = expected;
}

void Sema::declareVariable(const std::string& name, const Symbol& sym, const ASTNode* at) {
    if (declare(name, sym, at)) declareFields(name, sym);
}

// Struct değişkeninin her alanı, değişkenin kendisiyle aynı özelliklerde
// (kapsam, parametre olup olmadığı) ayrı bir sembol olur.
void Sema::declareFields(const std::string& name, const Symbol& owner) {
    if (!owner.type.isStruct()) return;
    for (size_t i = 0; i < owner.type.sub.size(); ++i) {
        Symbol field = owner;
        field.type = owner.type.sub[i];
        // Parametrenin matris alanı (ve matris içeren alt struct'ı) salt okunurdur.
        field.readOnly = owner.isParam && field.type.containsMatrix();
        field.scopeDepth = static_cast<int>(scopes.size()) - 1;

        const std::string fieldName = name + "." + owner.type.fieldNames[i];
        scopes.back().emplace(fieldName, field);
        declareFields(fieldName, field);
    }
}
