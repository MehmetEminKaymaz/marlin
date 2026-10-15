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
