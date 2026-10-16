#include "Sema.h"

#include <algorithm>
#include <cstdint>

void Sema::error(const ASTNode* node, const std::string& msg) {
    // Mesajlardaki tanım adları kullanıcının yazacağı biçimde gösterilir:
    // "araclar$vektor$norm" → "araclar.vektor.norm".
    std::string text = msg;
    std::replace(text.begin(), text.end(), '$', '.');
    errors.push_back(program->modules[node->module].path + ":" + std::to_string(node->line) +
                     ":" + std::to_string(node->column) + ": hata: " + text);
}

// ===========================================================================
// Kapsam
// ===========================================================================

bool Sema::declare(const std::string& name, const Symbol& sym, const ASTNode* at) {
    auto& scope = scopes.back();
    if (scope.count(name)) {
        error(at, "'" + name + "' bu kapsamda zaten tanimli");
        return false;
    }
    Symbol stored = sym;
    stored.scopeDepth = static_cast<int>(scopes.size()) - 1;
    scope.emplace(name, stored);
    return true;
}

Sema::Symbol* Sema::findGlobal(const std::string& linkName) {
    auto found = scopes.front().find(linkName);
    return found != scopes.front().end() ? &found->second : nullptr;
}

Sema::Symbol* Sema::lookup(const std::string& name) {
    // Yerel kapsamlar (scopes[0] globaldir; ona modül üzerinden bakılır).
    // Noktalı isimde (p.x) aranan, kökün (p) tanımlandığı kapsamdaki alandır:
    // içteki bir `p`, dıştaki struct `p`nin alanlarını da gölgeler.
    const std::string root = name.substr(0, name.find('.'));
    for (size_t i = scopes.size(); i-- > 1;) {
        if (!scopes[i].count(root)) continue;
        auto found = scopes[i].find(name);
        return found != scopes[i].end() ? &found->second : nullptr;
    }
    return findGlobal(globalLinkName(name));
}

// Kaynaktaki ismin global tanımdaki adı; bulunamazsa boş. Önce isim olduğu
// gibi çözülür (fonksiyon, global, `modul.isim`). Olmazsa noktalı ismin bir
// global struct'ın alanı olup olmadığına bakılır: `havuz.hata`, kökü
// (`havuz` ya da `db.havuz`) global bir struct olan alan yoludur.
std::string Sema::globalLinkName(const std::string& name) {
    const std::string whole = linkNameOf(name);
    if (!whole.empty() && findGlobal(whole)) return whole;
    for (size_t dot = name.find('.'); dot != std::string::npos; dot = name.find('.', dot + 1)) {
        const std::string root = linkNameOf(name.substr(0, dot));
        if (root.empty()) continue;
        Symbol* owner = findGlobal(root);
        if (!owner || owner->kind != Symbol::Global || !owner->type.isStruct()) continue;
        return root + name.substr(dot);
    }
    return whole;
}

Sema::Symbol* Sema::lookup(const Identifier* ident) {
    // Global ya da fonksiyona çözülmüş düğümün adı zaten tanımın adıdır.
    if (ident->ref == Identifier::Ref::Global || ident->ref == Identifier::Ref::Function)
        return findGlobal(ident->name);
    return lookup(ident->name);
}

// ===========================================================================
// Modüller
// ===========================================================================

std::string Sema::linkNameOf(const std::string& name) const {
    const ModuleNames& names = moduleNames[currentModule];

    const size_t dot = name.rfind('.');
    if (dot != std::string::npos) { // modul.isim
        auto alias = names.aliases.find(name.substr(0, dot));
        if (alias == names.aliases.end()) return "";
        return program->modules[alias->second].prefix + name.substr(dot + 1);
    }
    auto imported = names.imported.find(name);
    if (imported != names.imported.end()) return imported->second;
    return program->modules[currentModule].prefix + name;
}

// Bütün modüllerin üst seviye tanımlarının (fonksiyon, global, struct) adlarını
// toplar. Tipler ve gövdeler çözülmeden önce yapılır: import'ların bağlanması
// ve struct adlarının tip olarak tanınması bu tabloya dayanır.
void Sema::collectTopLevel() {
    for (auto& decl : program->declarations) {
        const std::string* name = nullptr;
        bool isPublic = false;
        auto* st = dynamic_cast<StructDecl*>(decl.get());
        if (auto* fn = dynamic_cast<FunctionDecl*>(decl.get())) {
            name = &fn->name;
            isPublic = fn->isPublic;
        } else if (auto* var = dynamic_cast<VariableDecl*>(decl.get())) {
            name = &var->name;
            isPublic = var->isPublic;
        } else if (st) {
            name = &st->name;
            isPublic = st->isPublic;
        }
        if (!name) continue;

        const bool inserted = topLevel.emplace(*name, TopLevel{decl->module, isPublic}).second;
        // (İki fonksiyon/global arasındaki çakışmayı `declare` raporlar.)
        if (st && !inserted) error(st, "'" + st->name + "' bu kapsamda zaten tanimli");
        else if (st) structs[st->name].decl = st;
        else if (!inserted && structs.count(*name))
            error(decl.get(), "'" + *name + "' bu kapsamda zaten tanimli (ayni adda bir struct var)");
    }
}

// Her modülün import'larını isimlere bağlar; `from` ile istenen ismin varlığı,
// görünürlüğü ve modülün kendi tanımlarıyla çakışması denetlenir.
void Sema::bindImports() {
    moduleNames.assign(program->modules.size(), {});
    for (size_t m = 0; m < program->modules.size(); ++m) {
        const Module& self = program->modules[m];
        ModuleNames& names = moduleNames[m];

        for (auto& decl : self.imports) {
            if (decl->target < 0) continue;
            const Module& target = program->modules[decl->target];

            if (!decl->isFrom) {
                const std::string& alias = decl->alias.empty() ? decl->modulePath : decl->alias;
                auto [it, inserted] = names.aliases.emplace(alias, decl->target);
                if (!inserted && it->second != decl->target)
                    error(decl.get(), "'" + alias + "' adi baska bir module verilmis");
                continue;
            }
            for (const ImportDecl::Name& item : decl->names) {
                const std::string& local = item.alias.empty() ? item.name : item.alias;
                const std::string linkName = target.prefix + item.name;
                auto found = topLevel.find(linkName);
                if (found == topLevel.end()) {
                    error(decl.get(), "'" + decl->modulePath + "' modulunde '" + item.name +
                                      "' adinda bir tanim yok");
                    continue;
                }
                if (!found->second.isPublic && found->second.module != static_cast<int>(m)) {
                    error(decl.get(), "'" + decl->modulePath + "' modulundeki '" + item.name +
                                      "' private; import edilemez (tanimina 'public' yazin)");
                    continue;
                }
                if (topLevel.count(self.prefix + local)) {
                    error(decl.get(), "'" + local + "' bu modulde zaten tanimli; 'as' ile "
                                      "baska bir ad verin");
                    continue;
                }
                auto [it, inserted] = names.imported.emplace(local, linkName);
                if (!inserted && it->second != linkName)
                    error(decl.get(), "'" + local + "' adi birden cok tanim icin kullanilmis; "
                                      "'as' ile baska bir ad verin");
            }
        }
    }
}

void Sema::reportUndefined(const Identifier* ident) {
    const std::string& name = ident->name;
    if (structs.count(linkNameOf(name))) {
        error(ident, "'" + name + "' bir struct tipi; deger olarak kullanilamaz (olusturmak icin: " +
                     name + "(...))");
        return;
    }

    // p.x.y — kök yerel bir değişkense sorun alanlardadır: alanı olmayan bir
    // değişken ya da struct'ta bulunmayan bir alan.
    const size_t firstDot = name.find('.');
    if (firstDot != std::string::npos) {
        std::string prefix = name.substr(0, firstDot);
        for (size_t i = scopes.size(); i-- > 1;) {
            auto root = scopes[i].find(prefix);
            if (root == scopes[i].end()) continue;

            const Symbol* owner = &root->second;
            size_t begin = firstDot + 1;
            while (true) {
                const size_t end = name.find('.', begin);
                const std::string field = name.substr(begin, end - begin);
                if (!owner->type.isStruct()) {
                    if (!owner->type.isError())
                        error(ident, "'" + prefix + "' bir struct degil (tipi: " + owner->type.str() +
                                     "); '." + field + "' ile alanina erisilemez");
                    return;
                }
                auto next = scopes[i].find(prefix + "." + field);
                if (next == scopes[i].end()) {
                    error(ident, "'" + owner->type.str() + "' tipinde '" + field + "' adinda bir alan yok");
                    return;
                }
                owner = &next->second;
                prefix += "." + field;
                if (end == std::string::npos) return;
                begin = end + 1;
            }
        }
    }

    const size_t dot = name.rfind('.');
    if (dot == std::string::npos) {
        error(ident, "tanimsiz isim: '" + name + "'");
        return;
    }
    const std::string module = name.substr(0, dot);
    if (!moduleNames[currentModule].aliases.count(module))
        error(ident, "tanimsiz modul: '" + module + "' (import edilmemis)");
    else
        error(ident, "'" + module + "' modulunde '" + name.substr(dot + 1) + "' adinda bir tanim yok");
}

// ===========================================================================
// Tipler
// ===========================================================================

SemType Sema::resolveType(Type* t) {
    if (!t) return SemType::Error();
    switch (t->kind) {
        case TypeKind::Int8:    return SemType::Int(8);
        case TypeKind::Int16:   return SemType::Int(16);
        case TypeKind::Int32:   return SemType::Int(32);
        case TypeKind::Int64:   return SemType::Int(64);
        case TypeKind::UInt8:   return SemType::Int(8, false);
        case TypeKind::UInt16:  return SemType::Int(16, false);
        case TypeKind::UInt32:  return SemType::Int(32, false);
        case TypeKind::UInt64:  return SemType::Int(64, false);
        case TypeKind::Float32: return SemType::Float(32);
        case TypeKind::Float64: return SemType::Float(64);
        case TypeKind::Bool:    return SemType::Bool();
        case TypeKind::Char:    return SemType::Char();
        case TypeKind::String:  return SemType::String();
        case TypeKind::Void:    return SemType::Void();
        case TypeKind::Array: {
            SemType elem = resolveType(t->elementType.get());
            if (elem.isError()) return elem;
            if (!elem.isScalar() && !elem.isString()) {
                error(t, "dizi elemani sayisal, bool, char ya da string olmali (bulunan: " + elem.str() + ")");
                return SemType::Error();
            }
            if (t->arraySize <= 0) {
                error(t, "dizi boyutu pozitif olmali");
                return SemType::Error();
            }
            return SemType::Array(elem, t->arraySize);
        }
        case TypeKind::List: {
            SemType elem = resolveType(t->elementType.get());
            if (elem.isError()) return elem;
            if (!elem.isScalar() && !elem.isString() && !elem.isStruct() && !elem.isFunction() &&
                !elem.isTask()) {
                error(t, "liste elemani sayisal, bool, char, string, fonksiyon, task ya da struct olmali (bulunan: " + elem.str() + ")");
                return SemType::Error();
            }
            return SemType::List(elem);
        }
        case TypeKind::Matrix: {
            SemType elem = resolveType(t->elementType.get());
            if (elem.isError()) return elem;
            if (!elem.isNumeric()) {
                error(t, "matris elemani sayisal olmali (bulunan: " + elem.str() + ")");
                return SemType::Error();
            }
            if (t->rank < 1) {
                error(t, "matrisin boyut sayisi 1 ya da daha buyuk bir tamsayi olmali "
                         "(or. matrix<float, 3>)");
                return SemType::Error();
            }
            return SemType::Matrix(elem, t->rank);
        }
        case TypeKind::Lambda: {
            std::vector<SemType> params;
            bool ok = true;
            for (auto& p : t->paramTypes) {
                params.push_back(resolveValueType(p.get()));
                ok = ok && !params.back().isError();
            }
            SemType ret = resolveReturnType(t->returnType.get());
            if (!ok || ret.isError()) return SemType::Error();
            return SemType::Function(ret, params);
        }
        case TypeKind::Task: {
            SemType result = resolveType(t->elementType.get());
            if (result.isError()) return result;
            if (!taskResultAllowed(result)) {
                error(t, "gorev sonucu olarak " + result.str() + " henuz desteklenmiyor (sayi, bool, "
                         "char, string, fonksiyon, void ya da bunlarin listesi olabilir)");
                return SemType::Error();
            }
            return SemType::Task(result);
        }
        case TypeKind::Named:
            return resolveNamedType(t);
    }
    return SemType::Error();
}

SemType Sema::resolveValueType(Type* t) {
    SemType type = resolveType(t);
    if (type.isVoid()) {
        error(t, "void bir deger tipi olarak kullanilamaz");
        return SemType::Error();
    }
    return type;
}

SemType Sema::resolveReturnType(Type* t) {
    SemType type = resolveType(t);
    if (type.isArray()) {
        error(t, "fonksiyonlar dizi donduremez (dizi yigitta yasar); diziyi parametre olarak alin");
        return SemType::Error();
    }
    return type;
}

// İki sayısal tipin ortak tipi: kayan nokta varsa kayan nokta, yoksa geniş
// olan tamsayı. Eşit genişlikte işaretli/işaretsiz karışırsa işaretsiz.
SemType Sema::commonType(const SemType& a, const SemType& b) {
    if (a.isFloat() || b.isFloat()) {
        unsigned width = 0;
        if (a.isFloat()) width = std::max(width, a.width);
        if (b.isFloat()) width = std::max(width, b.width);
        return SemType::Float(width);
    }
    if (a.width != b.width) return a.width > b.width ? a : b;
    return SemType::Int(a.width, a.isSigned && b.isSigned);
}

// Sayı literali ya da onun eksilisi: tipi bağlamdan alabilen ifadeler.
bool Sema::isLiteralLike(const Expression* e) {
    if (dynamic_cast<const IntegerLiteral*>(e) || dynamic_cast<const FloatLiteral*>(e))
        return true;
    if (auto* u = dynamic_cast<const UnaryExpr*>(e))
        return u->op == UnaryExpr::Negate && isLiteralLike(u->operand.get());
    return false;
}

void Sema::retypeLiteral(Expression* e, const SemType& t) {
    e->type = t;
    if (auto* u = dynamic_cast<UnaryExpr*>(e))
        retypeLiteral(u->operand.get(), t);
}

// Tamsayı literali hedef tamsayı tipine sığıyor mu?
bool Sema::literalFits(const Expression* e, const SemType& t) {
    if (!t.isInt() || t.width >= 64) return true;
    bool negated = false;
    while (auto* u = dynamic_cast<const UnaryExpr*>(e)) {
        negated = !negated;
        e = u->operand.get();
    }
    auto* lit = dynamic_cast<const IntegerLiteral*>(e);
    if (!lit) return true;

    const int64_t value = lit->value;
    if (negated) return t.isSigned && value <= (int64_t(1) << (t.width - 1));
    const int64_t max = t.isSigned ? (int64_t(1) << (t.width - 1)) - 1
                                   : (int64_t(1) << t.width) - 1;
    return value <= max;
}

SemType Sema::check(ExprPtr& e) {
    e->accept(*this);
    checkGpuExpr(e.get());
    return e->type;
}

// check ile aynı, ama bağlamın beklediği tipi bilir. Tek başına tipi
// çıkarılamayan ifadeler (boş liste literali `[]`) tipini buradan alır.
SemType Sema::checkExpecting(ExprPtr& e, const SemType& expected) {
    auto* list = dynamic_cast<ListLiteral*>(e.get());
    if (list && list->elements.empty() && expected.isList()) {
        list->type = expected;
        return expected;
    }
    // Beklenen tip, ifadenin kendi visit metoduna ipucu olarak bırakılır
    // ([[1, 2], [3, 4]] ve zeros(r, c) matrisin eleman tipini buradan öğrenir).
    hint = expected;
    SemType type = check(e);
    hint = SemType::Error();
    return type;
}

SemType Sema::takeHint() {
    SemType taken = hint;
    hint = SemType::Error();
    return taken;
}

static std::unique_ptr<Expression> wrapInCast(std::unique_ptr<Expression> operand,
                                              const SemType& target) {
    auto cast = std::make_unique<CastExpr>(std::move(operand));
    cast->line = cast->operand->line;
    cast->column = cast->operand->column;
    cast->module = cast->operand->module;
    cast->type = target;
    return cast;
}

// `slot`taki ifadeyi `target` tipine getirir: literalse tipini değiştirir,
// sayısal dönüşümse CastExpr ile sarar, aksi halde hata verir.
void Sema::coerce(ExprPtr& slot, const SemType& target) {
    const SemType src = slot->type;
    if (src.isError() || target.isError() || src == target) return;

    if (src.isNumeric() && target.isNumeric()) {
        // Tamsayı literali her sayısal tipe, kayan nokta literali kayan nokta
        // tiplerine doğrudan o tipte üretilir.
        if (isLiteralLike(slot.get()) && (src.isInt() || target.isFloat())) {
            if (!literalFits(slot.get(), target))
                error(slot.get(), "sabit deger " + target.str() + " tipine sigmiyor");
            retypeLiteral(slot.get(), target);
            return;
        }
        slot = wrapInCast(std::move(slot), target);
        return;
    }

    // matrix<int> → matrix<float>: eleman eleman dönüşüm. Boyut sayısı aynı
    // olmalı; farklıysa aşağıda tip uyuşmazlığı olarak raporlanır.
    if (src.isMatrix() && target.isMatrix() && src.rank == target.rank) {
        slot = wrapInCast(std::move(slot), target);
        return;
    }

    // [1, 2, 3] literali bağlama göre biçimlenir:
    //   * eleman tipi farklı ama boyutu aynı bir diziye   (int64[3] a = [1, 2, 3])
    //   * herhangi uzunlukta bir listeye                   (list<int> l = [1, 2, 3])
    if (auto* list = dynamic_cast<ListLiteral*>(slot.get()); list && src.isArray()) {
        const bool sizeOk = target.isList() || (target.isArray() && src.arraySize == target.arraySize);
        if (sizeOk) {
            const SemType& from = src.elementType();
            const SemType& to = target.elementType();
            if (from == to || (from.isNumeric() && to.isNumeric())) {
                for (auto& elem : list->elements) coerce(elem, to);
                list->type = target;
                return;
            }
        }
    }

    error(slot.get(), "tip uyusmazligi: beklenen " + target.str() + ", bulunan " + src.str());
}

// İkili işlemin iki tarafını ortak bir sayısal tipe getirir.
SemType Sema::unify(ExprPtr& l, ExprPtr& r) {
    return unifyAll({&l, &r});
}

// Verilen sayısal ifadeleri ortak tipe getirir. Literaller diğer ifadelerin
// tipine uyar (i64 + 1 → 1 de i64 olur); kayan nokta literali bir tamsayıyla
// karşılaşırsa ortak tip kayan nokta olur.
SemType Sema::unifyAll(const std::vector<ExprPtr*>& parts) {
    SemType common;
    bool haveTyped = false;
    for (ExprPtr* p : parts) {
        if (isLiteralLike(p->get())) continue;
        common = haveTyped ? commonType(common, (*p)->type) : (*p)->type;
        haveTyped = true;
    }
    for (ExprPtr* p : parts) {
        if (!isLiteralLike(p->get())) continue;
        const SemType& lit = (*p)->type;
        if (!haveTyped) { common = lit; haveTyped = true; }
        else if (lit.isFloat() && !common.isFloat()) common = commonType(common, lit);
    }
    for (ExprPtr* p : parts) coerce(*p, common);
    return common;
}

void Sema::checkCondition(ExprPtr& cond, const char* what) {
    SemType t = check(cond);
    if (!t.isError() && !t.isBool())
        error(cond.get(), std::string(what) + " kosulu bool olmali (bulunan: " + t.str() + ")");
}

// ===========================================================================
// Program ve bildirimler
// ===========================================================================

void Sema::visit(Program* node) {
    program = node;
    scopes.clear();
    scopes.emplace_back(); // global kapsam: bütün modüllerin tanımları

    // Önce isimler: üst seviye tanımlar toplanır ve import'lar bağlanır.
    // Tiplerin çözülmesi buna dayanır (struct adları, başka modülün tipleri).
    collectTopLevel();
    bindImports();
    for (auto& entry : structs) resolveStruct(entry.second);

    // Fonksiyonlar ve globaller, gövdelerden önce bildirilir; böylece kaynak
    // sırasından (ve hangi modülde olduklarından) bağımsız olarak birbirlerine
    // erişebilirler.
    for (auto& decl : node->declarations)
        if (auto* fn = dynamic_cast<FunctionDecl*>(decl.get())) {
            currentModule = fn->module; // imzadaki tip adları bu modüle göre çözülür
            declareFunction(fn);
        }
    for (auto& decl : node->declarations)
        if (auto* var = dynamic_cast<VariableDecl*>(decl.get())) {
            currentModule = var->module;
            checkGlobal(var);
        }
    for (auto& decl : node->declarations)
        if (auto* fn = dynamic_cast<FunctionDecl*>(decl.get()); fn && !fn->isExtern)
            decl->accept(*this);

    // Paralel döngülerden çağrılan fonksiyonlar ve görevler, ancak bütün
    // fonksiyonlar görüldükten sonra denetlenebilir.
    checkParallelCalls();
    checkTasks();
    markTemporaryListArguments();

    // Giriş noktası, işletim sisteminin çağırabileceği biçimde olmalı. Yalnızca
    // kök modülün (derlenen dosyanın) `main`i giriş noktasıdır.
    const std::string rootError = node->modules[0].path + ": hata: ";
    Symbol* mainFn = findGlobal("main");
    if (!mainFn || mainFn->kind != Symbol::Function) {
        errors.push_back(rootError + "programda 'main' fonksiyonu yok");
    } else if (mainFn->isGpu) {
        errors.push_back(rootError + "'main' @GPU olamaz; program CPU'da baslar");
    } else if (!mainFn->type.returnType().isError()) {
        const SemType& ret = mainFn->type.returnType();
        if (mainFn->type.paramCount() != 0 || !(ret.isVoid() || ret == SemType::Int(32)))
            errors.push_back(rootError + "'main' parametresiz olmali ve void ya da int dondurmeli");
    }
}

std::vector<SemType> Sema::resolveParams(const std::vector<std::unique_ptr<ParamDecl>>& params) {
    std::vector<SemType> types;
    for (auto& p : params) types.push_back(resolveValueType(p->type.get()));
    return types;
}

void Sema::declareParams(const std::vector<std::unique_ptr<ParamDecl>>& params,
                         const std::vector<SemType>& types) {
    for (size_t i = 0; i < params.size(); ++i) {
        Symbol param{types[i], Symbol::Variable, funcDepth,
                     /*readOnly=*/types[i].containsMatrix(), /*isParam=*/true};
        param.paramIndex = static_cast<int>(i);
        declareVariable(params[i]->name, param, params[i].get());
    }
}

void Sema::declareFunction(FunctionDecl* fn) {
    std::vector<SemType> params = resolveParams(fn->params);
    SemType ret = resolveReturnType(fn->returnType.get());
    fn->fnType = SemType::Function(ret, params);

    if (fn->isExtern) checkExtern(fn, params, ret);
    else resolveTarget(fn);
    if (fn->isGpu) {
        for (size_t i = 0; i < params.size(); ++i) checkGpuType(params[i], fn->params[i].get());
        checkGpuType(ret, fn->returnType.get());
    }
    Symbol symbol{fn->fnType, Symbol::Function, 0};
    symbol.isGpu = fn->isGpu;
    symbol.module = fn->module;
    symbol.isPublic = fn->isPublic;
    symbol.isExtern = fn->isExtern;
    declare(fn->name, symbol, fn);
    functionDecls[fn->name] = fn;
}

// extern func: gövdesi çalışma zamanı kütüphanesinde, C ile yazılı.
//
//   * Yalnızca standart kütüphane modüllerinde yazılabilir; kullanıcı kodu
//     C'ye doğrudan açılmaz. Her zaman private'tır (gramer `public` kabul
//     etmez): dışarıya bir marlin fonksiyonuyla sarılarak açılır.
//   * İmzasında yalnızca C'de doğrudan karşılığı olan tipler bulunabilir.
//     Liste, matris ve struct döndürmesi gereken işlevler, extern'lerin
//     üzerine marlin ile yazılır.
//   * Derleyici gövdeyi göremediği için fonksiyonu yan etkili sayar (paralel
//     döngüden çağrılamaz); yan etkisi olmayanlar @pure ile işaretlenir.
//   * Her zaman @CPU'dur.
void Sema::checkExtern(FunctionDecl* fn, const std::vector<SemType>& params, const SemType& ret) {
    if (program->modules[fn->module].name.rfind("std.", 0) != 0)
        error(fn, "'extern' yalnizca standart kutuphane dosyalarinda kullanilabilir");

    if (fn->annotation == "pure") fn->isPure = true;
    else if (!fn->annotation.empty())
        error(fn, "extern fonksiyonda yalnizca @pure kullanilabilir (bulunan: @" + fn->annotation + ")");

    auto allowed = [](const SemType& t) {
        return t.isError() || t.isString() || t.isBool() || t.isChar() || t.isFloat() ||
               (t.isInt() && t.width >= 32);
    };
    const char* rule = "extern fonksiyonda yalnizca int, int64, float, float64, bool, char ve "
                       "string kullanilabilir (bulunan: ";
    for (size_t i = 0; i < params.size(); ++i)
        if (!allowed(params[i])) error(fn->params[i].get(), rule + params[i].str() + ")");
    if (!ret.isVoid() && !allowed(ret)) error(fn->returnType.get(), rule + ret.str() + ")");

    if (!fn->isPure)
        effects[fn->name].unsafeReason = "C ile yazilmis bir kutuphane fonksiyonu (yan etkisi olabilir)";
}

void Sema::checkGlobal(VariableDecl* v) {
    SemType type;
    if (v->type) type = resolveValueType(v->type.get());

    if (v->initializer) {
        Expression* init = v->initializer.get();
        bool constant = isLiteralLike(init) || dynamic_cast<BooleanLiteral*>(init) ||
                        dynamic_cast<CharLiteral*>(init);
        if (!constant) {
            error(init, "global degiskenin baslangic degeri sabit bir literal olmali");
            type = SemType::Error();
        } else {
            SemType initType = check(v->initializer);
            if (v->type) coerce(v->initializer, type);
            else type = initType;
        }
    }

    // Başlangıç değeri olmadan (sıfır, boş string) tanımlanabilenler: diziler,
    // stringler ve alanları sayı / bool / char / string olan struct'lar.
    // Değerleri fonksiyonlarda atanır.
    auto globalLeaf = [](const SemType& t) { return t.isScalar() || t.isString(); };
    bool structOk = type.isStruct();
    for (const SemLeaf& leaf : leavesOf(type)) structOk = structOk && globalLeaf(leaf.type);
    const bool withoutInit = !v->initializer &&
                             (type.isArray() || type.isString() || structOk);
    if (!type.isError() && !type.isScalar() && !withoutInit) {
        error(v, "global degisken olarak yalnizca sayisal/bool/char degerler; baslangic degeri "
                 "olmadan diziler, stringler ve alanlari sayisal/bool/char/string olan struct'lar "
                 "destekleniyor (bulunan: " + type.str() + ")");
        type = SemType::Error();
    }

    v->varType = type;
    Symbol symbol{type, Symbol::Global, 0};
    symbol.module = v->module;
    symbol.isPublic = v->isPublic;
    // Struct ise alanları da ("havuz.hata") global semboller olur.
    declareVariable(v->name, symbol, v);
}

void Sema::visit(FunctionDecl* node) {
    const SemType ret = node->fnType.returnType();

    currentModule = node->module; // gövdedeki isimler bu modüle göre çözülür
    funcDepth = 1;
    scopes.emplace_back();
    flow.clear();
    returnTypes.push_back(ret);
    parallelLoops.clear();
    currentEffects = &effects[node->name];
    currentFunction = node->name;
    gpuFunction = node->isGpu;
    if (node->hasMatrixSignature()) noteGpuWork("matris kullaniyor");

    std::vector<SemType> params(node->fnType.sub.begin() + 1, node->fnType.sub.end());
    declareParams(node->params, params);
    node->body->accept(*this);

    if (!ret.isVoid() && !ret.isError() && !alwaysReturns(node->body.get()))
        error(node, "'" + node->name + "' fonksiyonu her yolda deger dondurmuyor (" +
                    ret.str() + " bekleniyor)");

    currentEffects = nullptr;
    currentFunction.clear();
    gpuFunction = false;
    returnTypes.pop_back();
    scopes.pop_back();
    funcDepth = 0;
}

void Sema::visit(LambdaExpr* node) {
    if (gpuFunction) error(node, "@GPU fonksiyonda lambda kullanilamaz");
    const bool outerGpu = gpuFunction;
    gpuFunction = false;

    std::vector<SemType> params = resolveParams(node->params);
    SemType ret = resolveReturnType(node->returnType.get());

    // Lambda ayrı bir fonksiyondur: dıştaki döngü/if çerçeveleri onu ilgilendirmez.
    std::vector<FlowFrame> outerFlow = std::move(flow);
    flow.clear();
    // Aynı sebeple dıştaki paralel döngüler ve fonksiyonun etkileri de: lambda
    // ancak çağrılınca çalışır, o çağrı ise dolaylıdır ve ayrıca denetlenir.
    std::vector<ParallelLoop> outerParallel = std::move(parallelLoops);
    parallelLoops.clear();
    FunctionEffects* outerEffects = currentEffects;
    currentEffects = &effects[lambdaKey(node)];
    const std::string outerFunction = currentFunction;
    currentFunction = lambdaKey(node);
    ++funcDepth;
    scopes.emplace_back();
    returnTypes.push_back(ret);

    declareParams(node->params, params);
    if (node->exprBody) {
        check(node->exprBody);
        if (!ret.isVoid()) coerce(node->exprBody, ret);
    } else {
        node->blockBody->accept(*this);
        if (!ret.isVoid() && !ret.isError() && !alwaysReturns(node->blockBody.get()))
            error(node, "lambda her yolda deger dondurmuyor (" + ret.str() + " bekleniyor)");
    }

    returnTypes.pop_back();
    scopes.pop_back();
    --funcDepth;
    flow = std::move(outerFlow);
    parallelLoops = std::move(outerParallel);
    currentEffects = outerEffects;
    currentFunction = outerFunction;
    gpuFunction = outerGpu;

    bool ok = !ret.isError();
    for (auto& p : params) ok = ok && !p.isError();
    node->type = ok ? SemType::Function(ret, params) : SemType::Error();
    if (ok) noteValueFunction(lambdaKey(node), node->type, node, /*isLambda=*/true);
}

void Sema::visit(ParamDecl*) {}
void Sema::visit(Type*) {}

// ===========================================================================
// İfadeler
// ===========================================================================

// Varsayılan tip int; değer int'e sığmıyorsa int64. (Bağlam başka bir tip
// istiyorsa coerce literalin tipini sonradan değiştirir.)
void Sema::visit(IntegerLiteral* node) {
    node->type = SemType::Int(node->value > INT32_MAX ? 64 : 32);
}
void Sema::visit(FloatLiteral* node)   { node->type = SemType::Float(64); }
void Sema::visit(BooleanLiteral* node) { node->type = SemType::Bool(); }
void Sema::visit(CharLiteral* node)    { node->type = SemType::Char(); }
void Sema::visit(StringLiteral* node)  { node->type = SemType::String(); }
void Sema::visit(CastExpr*) {} // Sema'nın kendi eklediği düğüm; tipi zaten yazılı

void Sema::visit(Identifier* node) {
    const bool isAccessTarget = skipWholeRead;
    const bool isCallee = identIsCallee;
    const bool iterated = identIsIterated;
    const auto asArgument = argumentContext;
    skipWholeRead = identIsCallee = identIsIterated = false;
    argumentContext = {"", -1};
    node->type = SemType::Error();
    Symbol* sym = lookup(node);
    if (!sym) {
        if (lookupBuiltin(node->name) != Builtin::None)
            error(node, "yerlesik fonksiyon '" + node->name + "' yalnizca cagrilabilir; deger olarak kullanilamaz");
        else
            reportUndefined(node);
        return;
    }
    bool isLocal = sym->kind == Symbol::Variable || sym->kind == Symbol::Iterator;
    if (isLocal && sym->funcDepth != funcDepth) {
        error(node, "lambda, dis kapsamdaki '" + node->name +
                    "' degiskenini kullanamaz (yakalama henuz desteklenmiyor); parametre olarak verin");
        return;
    }
    // Başka modülün tanımı yalnızca public ise kullanılabilir (v.gizli).
    if (!isLocal && !sym->isPublic && sym->module != currentModule) {
        error(node, "'" + node->name + "' private; baska bir modulden kullanilamaz "
                    "(tanimina 'public' yazin)");
        return;
    }
    // extern fonksiyonun marlin tarafında bir gövdesi yoktur; değer olarak dolaşamaz.
    if (sym->isExtern && !isCallee) {
        error(node, "extern fonksiyon '" + node->name + "' deger olarak kullanilamaz; "
                    "yalnizca cagrilabilir");
        return;
    }
    // Global ve fonksiyonlarda kaynaktaki ad ("v.norm") tanımın adıyla değişir.
    if (!isLocal && node->ref == Identifier::Ref::Unresolved) node->name = globalLinkName(node->name);
    node->ref = sym->kind == Symbol::Function ? Identifier::Ref::Function
              : sym->kind == Symbol::Global   ? Identifier::Ref::Global
                                              : Identifier::Ref::Local;
    node->type = sym->type;
    if (sym->kind == Symbol::Function && !isCallee)
        noteValueFunction(node->name, sym->type, node, /*isLambda=*/false);
    // Liste parametresinin kullanımı (bkz. SemaBorrow.cpp).
    if (sym->isParam && sym->paramIndex >= 0 && sym->type.isList() &&
        node->name.find('.') == std::string::npos) {
        ListParamUse& use = listParamUses[currentFunction][sym->paramIndex];
        if (isAccessTarget || iterated || asArgument.second == -2) {
            // ödünç: indeksleme, uzunluk, for ... in, yerleşik / göreve kopya
        } else if (!asArgument.first.empty()) {
            use.passes.push_back(asArgument);
        } else {
            use.escapes = true;
        }
    }
    checkGpuIdentifier(node, *sym, isCallee);
    // (İndeksleme ya da atama hedefi olarak kullanım ayrıca kaydedilir.)
    if (!isAccessTarget) noteWholeRead(node, *sym);
}

// Matris literali — beklenen tip matris olduğunda. İç içelik derinliği
// matrisin boyut sayısı kadar olmalıdır:
//
//   matrix<int, 1>   [1, 2, 3]
//   matrix<int>      [[1, 2], [3, 4]]
//   matrix<int, 3>   [[[1, 2], [3, 4]], [[5, 6], [7, 8]]]
//
// Aynı seviyedeki bütün alt listeler eşit uzunlukta olmalı; en içteki
// elemanlar matrisin eleman tipine getirilir.
void Sema::checkMatrixLiteral(ListLiteral* node, const SemType& matrixType) {
    std::vector<size_t> shape; // her seviyede görülen ilk uzunluk
    if (checkMatrixLiteralLevel(node, matrixType, 0, shape))
        node->type = matrixType;
}

bool Sema::checkMatrixLiteralLevel(ListLiteral* node, const SemType& matrixType, int depth,
                                   std::vector<size_t>& shape) {
    const SemType& elemType = matrixType.elementType();
    const bool innermost = depth == matrixType.rank - 1;
    bool ok = true;

    if (node->elements.empty()) {
        error(node, "matris literalinde bos liste olamaz");
        return false;
    }
    if (shape.size() <= static_cast<size_t>(depth)) {
        shape.push_back(node->elements.size());
    } else if (node->elements.size() != shape[depth]) {
        error(node, "matris satirlari ayni uzunlukta olmali (beklenen " + std::to_string(shape[depth]) +
                    ", bulunan " + std::to_string(node->elements.size()) + ")");
        ok = false;
    }

    for (auto& elem : node->elements) {
        auto* inner = dynamic_cast<ListLiteral*>(elem.get());
        if (innermost) {
            if (inner) {
                error(elem.get(), matrixType.str() + " icin fazla ic ice liste: burada sayi bekleniyor");
                ok = false;
                continue;
            }
            SemType t = check(elem);
            if (t.isError()) { ok = false; continue; }
            if (!t.isNumeric()) {
                error(elem.get(), "matris elemani sayisal olmali (bulunan: " + t.str() + ")");
                ok = false;
                continue;
            }
            coerce(elem, elemType);
        } else {
            if (!inner) {
                error(elem.get(), matrixType.str() + " " + std::to_string(matrixType.rank) +
                                  " seviye ic ice liste bekler; burada liste bekleniyor");
                ok = false;
                continue;
            }
            ok = checkMatrixLiteralLevel(inner, matrixType, depth + 1, shape) && ok;
            // Alt liste, bir boyut eksik bir matris dilimidir.
            inner->type = SemType::Matrix(elemType, matrixType.rank - depth - 1);
        }
    }
    return ok;
}

void Sema::visit(ListLiteral* node) {
    const SemType expected = takeHint();
    node->type = SemType::Error();
    if (node->elements.empty()) {
        error(node, "bos liste literalinin tipi cikarilamiyor; tipi belirtin (or. list<int> l = [];)");
        return;
    }

    // Bağlam matris bekliyorsa literal matris olarak okunur.
    if (expected.isMatrix()) return checkMatrixLiteral(node, expected);

    // Bütün elemanlar liste literaliyse bu iç içe bir literaldir; tipi
    // bilinmeden kaç boyutlu bir matris olduğu çıkarılmaz.
    const bool nested = std::all_of(node->elements.begin(), node->elements.end(),
        [](const ExprPtr& e) { return dynamic_cast<ListLiteral*>(e.get()) != nullptr; });
    if (nested) {
        error(node, "ic ice liste literali yalnizca matris olarak kullanilabilir; tipi belirtin "
                    "(or. matrix<int> m = [[1, 2], [3, 4]];)");
        return;
    }

    // [Nokta(1, 2), Nokta(3, 4)] — struct listesi; [f, g] — fonksiyon listesi.
    // Struct ve fonksiyon dizisi olmadığı için bu literaller yalnızca liste
    // beklenen yerde yazılabilir.
    const bool wantsStructs = expected.isList() &&
        (expected.elementType().isStruct() || expected.elementType().isFunction());
    bool ok = true, allNumeric = true, anyStruct = false;
    for (auto& elem : node->elements) {
        SemType t = wantsStructs ? checkExpecting(elem, expected.elementType()) : check(elem);
        if (t.isError()) { ok = false; continue; }
        if (t.isStruct() || t.isFunction()) {
            anyStruct = true;
            continue;
        }
        if (!t.isScalar() && !t.isString()) {
            error(elem.get(), "dizi elemani sayisal, bool, char ya da string olmali (bulunan: " + t.str() + ")");
            ok = false;
        }
        allNumeric = allNumeric && t.isNumeric();
    }
    if (!ok) return;

    if (anyStruct) {
        const SemType elemType = node->elements.front()->type;
        for (auto& elem : node->elements) {
            if (elem->type == elemType) continue;
            error(elem.get(), "liste elemanlari ayni tipte olmali: beklenen " + elemType.str() +
                              ", bulunan " + elem->type.str());
            return;
        }
        if (!expected.isList()) {
            error(node, std::string(elemType.isStruct() ? "struct" : "fonksiyon") +
                        " listesi literalinin tipi belirtilmeli (or. list<" + elemType.str() +
                        "> l = [...];)");
            return;
        }
        node->type = SemType::List(elemType);
        return;
    }

    SemType elemType;
    if (allNumeric) {
        std::vector<ExprPtr*> parts;
        for (auto& elem : node->elements) parts.push_back(&elem);
        elemType = unifyAll(parts);
    } else {
        elemType = node->elements.front()->type;
        for (auto& elem : node->elements) {
            if (elem->type != elemType) {
                error(elem.get(), "dizi elemanlari ayni tipte olmali: beklenen " + elemType.str() +
                                  ", bulunan " + elem->type.str());
                return;
            }
        }
    }
    node->type = SemType::Array(elemType, static_cast<int64_t>(node->elements.size()));
}

void Sema::visit(ArrayAccessExpr* node) {
    const bool isWrite = accessIsWrite;
    accessIsWrite = false;
    node->type = SemType::Error();

    skipWholeRead = dynamic_cast<Identifier*>(node->target.get()) != nullptr;
    SemType target = check(node->target);
    skipWholeRead = false;
    for (auto& index : node->indices) {
        SemType t = check(index);
        if (!t.isError() && !t.isInt())
            error(index.get(), "indeks tamsayi olmali (bulunan: " + t.str() + ")");
    }

    if (target.isError()) return;
    if (!target.isArray() && !target.isList() && !target.isMatrix() && !target.isString()) {
        error(node->target.get(), "indekslenen ifade dizi, liste, matris ya da string degil (bulunan: " + target.str() + ")");
        return;
    }

    // Dizi ve liste tek indeksle (a[i]), matris boyut sayısı kadar indeksle
    // (m[i, j], t[i, j, k]) okunur.
    const size_t expectedCount = target.isMatrix() ? static_cast<size_t>(target.rank) : 1;
    if (node->indices.size() != expectedCount) {
        error(node, target.str() + " " + std::to_string(expectedCount) + " indeks bekler (verilen: " +
                    std::to_string(node->indices.size()) + ")");
        return;
    }

    // s[i]: stringin i. baytı. Stringler değişmez; yalnızca okunur.
    if (target.isString()) {
        if (isWrite)
            error(node, "string degistirilemez (s[i] yalnizca okunur); yeni bir string olusturun");
        else
            node->type = SemType::Char();
        return;
    }

    // Dizide sabit indeks derleme zamanında denetlenebilir. (Liste ve
    // matrisin boyutu çalışırken belli olur; onlar çalışma zamanında denetlenir.)
    if (auto* lit = dynamic_cast<IntegerLiteral*>(node->indices[0].get()))
        if (target.isArray() && lit->value >= target.arraySize)
            error(lit, "indeks " + std::to_string(lit->value) + " dizi sinirlarinin disinda (boyut " +
                       std::to_string(target.arraySize) + ")");
    node->type = target.elementType();
    noteElementAccess(node, isWrite);
    checkGpuElementAccess(node);
}

// Yerleşik fonksiyon çağrısı: argüman sayısını ve tiplerini denetler,
// argümanları işlemin beklediği tipe getirir ve sonuç tipini yazar.
//
// `expected`: bağlamın beklediği tip (yoksa Error). zeros / ones / fill
// matrisin eleman tipini buradan alır: `matrix<float> a = zeros(2, 3);`
void Sema::checkBuiltinCall(FunctionCallExpr* node, const std::string& name,
                            const SemType& expected) {
    auto& args = node->arguments;
    checkGpuBuiltin(node, name);
    // Yalnızca boyut soran yerleşikler (len, rows, cols, shape) verinin
    // elemanlarını okumaz; paralel döngü denetiminde okuma sayılmazlar.
    const bool onlyAsksSize = node->builtin == Builtin::Len || node->builtin == Builtin::Rows ||
                              node->builtin == Builtin::Cols || node->builtin == Builtin::Shape;
    bool ok = true;
    for (size_t i = 0; i < args.size(); ++i) {
        skipWholeRead = onlyAsksSize && i == 0 && dynamic_cast<Identifier*>(args[i].get());
        // Yerleşikler listeyi saklamaz (free hariç: listeyi bırakır).
        if (node->builtin != Builtin::Free && dynamic_cast<Identifier*>(args[i].get()))
            argumentContext = {"", -2};
        ok = !check(args[i]).isError() && ok;
        skipWholeRead = false;
        argumentContext = {"", -1};
    }

    auto expectMatrix = [&](size_t i) {
        if (args[i]->type.isMatrix()) return true;
        error(args[i].get(), "'" + name + "' matris bekler (bulunan: " + args[i]->type.str() + ")");
        return false;
    };
    auto expectRank2 = [&](size_t i) {
        if (!expectMatrix(i)) return false;
        if (args[i]->type.rank == 2) return true;
        error(args[i].get(), "'" + name + "' iki boyutlu matris bekler (bulunan: " +
                             args[i]->type.str() + ")");
        return false;
    };
    // İlk `count` argüman boyut uzunluğudur ve tamsayı olmalıdır.
    auto expectDims = [&](size_t count) {
        bool dimsOk = true;
        for (size_t i = 0; i < count; ++i) {
            if (args[i]->type.isInt()) continue;
            error(args[i].get(), "'" + name + "' boyut olarak tamsayi bekler (bulunan: " +
                                 args[i]->type.str() + ")");
            dimsOk = false;
        }
        return dimsOk;
    };

    auto expectArgs = [&](size_t count) {
        if (args.size() == count) return true;
        error(node, "'" + name + "' " + std::to_string(count) + " arguman bekler (verilen: " +
                    std::to_string(args.size()) + ")");
        return false;
    };
    auto expectNumeric = [&]() {
        bool numeric = true;
        for (auto& arg : args) {
            if (arg->type.isNumeric()) continue;
            error(arg.get(), "'" + name + "' sayisal bir deger bekler (bulunan: " + arg->type.str() + ")");
            numeric = false;
        }
        return numeric;
    };

    auto expectList = [&]() {
        if (args[0]->type.isList()) return true;
        error(args[0].get(), "'" + name + "' ilk arguman olarak liste bekler (bulunan: " +
                             args[0]->type.str() + ")");
        return false;
    };

    switch (node->builtin) {
        case Builtin::None:
            return;

        case Builtin::Print:
            node->type = SemType::Void();
            for (auto& arg : args) {
                const SemType& t = arg->type;
                if (t.isMatrix()) {
                    // Matris birden çok satıra yazılır; satır içine karıştırılamaz.
                    if (args.size() != 1)
                        error(arg.get(), "matris tek basina yazdirilmali: print(m)");
                } else if (!t.isError() && !t.isScalar() && !t.isString()) {
                    error(arg.get(), "bu tipte bir deger yazdirilamaz: " + t.str());
                }
            }
            return;

        // time(): bir başlangıç anından beri geçen süre, saniye. Tek başına
        // anlamı yoktur; iki çağrının farkı aradaki süreyi verir.
        case Builtin::Time:
            if (!expectArgs(0)) return;
            node->type = SemType::Float(64);
            return;

        // --- Matrisler ---

        // zeros(d1, ..., dn) / ones(...) / fill(d1, ..., dn, deger): her boyut
        // için bir uzunluk. Bağlam bir matris tipi bekliyorsa boyut sayısı ve
        // eleman tipi oradan gelir; yoksa argüman sayısından ve değerden.
        case Builtin::Zeros: case Builtin::Ones: case Builtin::Fill: {
            const bool isFill = node->builtin == Builtin::Fill;
            const size_t dimCount = args.size() - (isFill && !args.empty() ? 1 : 0);
            if (dimCount == 0) {
                error(node, "'" + name + "' en az bir boyut uzunlugu bekler");
                return;
            }
            if (expected.isMatrix() && dimCount != static_cast<size_t>(expected.rank)) {
                error(node, expected.str() + " icin '" + name + "' " + std::to_string(expected.rank) +
                            " boyut uzunlugu bekler (verilen: " + std::to_string(dimCount) + ")");
                return;
            }
            if (!ok || !expectDims(dimCount)) return;

            SemType elem = SemType::Float(64);
            if (isFill) {
                elem = args.back()->type;
                if (!elem.isNumeric()) {
                    error(args.back().get(), "'fill' sayisal bir deger bekler (bulunan: " + elem.str() + ")");
                    return;
                }
            }
            node->type = expected.isMatrix() ? expected
                                             : SemType::Matrix(elem, static_cast<int>(dimCount));
            if (isFill) coerce(args.back(), node->type.elementType());
            return;
        }

        // shape(m, i): i. boyutun uzunluğu. Boyut numarası sabit olmalıdır;
        // böylece geçerli olup olmadığı derlerken denetlenir.
        case Builtin::Shape: {
            if (!expectArgs(2) || !ok || !expectMatrix(0)) return;
            auto* which = dynamic_cast<IntegerLiteral*>(args[1].get());
            if (!which) {
                error(args[1].get(), "'shape' boyut numarasi olarak sabit bir tamsayi bekler (or. shape(m, 0))");
                return;
            }
            if (which->value >= args[0]->type.rank) {
                error(which, args[0]->type.str() + " icin boyut numarasi 0 ile " +
                             std::to_string(args[0]->type.rank - 1) + " arasinda olmali");
                return;
            }
            node->type = SemType::Int(64);
            return;
        }

        case Builtin::Rows: case Builtin::Cols:
            if (!expectArgs(1) || !ok || !expectRank2(0)) return;
            node->type = SemType::Int(64);
            return;

        case Builtin::Transpose:
            if (!expectArgs(1) || !ok || !expectRank2(0)) return;
            node->type = args[0]->type;
            return;

        case Builtin::Sum:
            if (!expectArgs(1) || !ok || !expectMatrix(0)) return;
            node->type = args[0]->type.elementType();
            return;

        case Builtin::Matmul: {
            // CPU'da matris çarpımı paralel döngüye açılıp çekirdeklere
            // dağıtılır (bkz. ParallelMatmul.h): paralel döngü gibi bekler.
            if (!gpuFunction && currentEffects && currentEffects->parallelReason.empty())
                currentEffects->parallelReason = "matmul kullaniyor (cekirdeklere dagitilir)";
            if (!expectArgs(2) || !ok) return;
            if (!expectRank2(0) | !expectRank2(1)) return;
            SemType elem = commonType(args[0]->type.elementType(), args[1]->type.elementType());
            node->type = SemType::Matrix(elem, 2);
            coerce(args[0], node->type);
            coerce(args[1], node->type);
            return;
        }

        // Tamsayı argüman float64'e çevrilir; float / float64 olduğu gibi kalır.
        case Builtin::Sqrt: case Builtin::Sin: case Builtin::Cos: case Builtin::Tan:
        case Builtin::Atan: case Builtin::Tanh: case Builtin::Exp: case Builtin::Log:
        case Builtin::Log2: case Builtin::Log10: case Builtin::Floor: case Builtin::Ceil:
        case Builtin::Round: case Builtin::Trunc:
            if (!expectArgs(1) || !ok || !expectNumeric()) return;
            if (!args[0]->type.isFloat()) coerce(args[0], SemType::Float(64));
            node->type = args[0]->type;
            return;

        case Builtin::Pow: case Builtin::Atan2: {
            if (!expectArgs(2) || !ok || !expectNumeric()) return;
            SemType common = unify(args[0], args[1]);
            if (!common.isFloat()) {
                common = SemType::Float(64);
                coerce(args[0], common);
                coerce(args[1], common);
            }
            node->type = common;
            return;
        }

        case Builtin::Abs:
            if (!expectArgs(1) || !ok || !expectNumeric()) return;
            node->type = args[0]->type;
            return;

        case Builtin::Min: case Builtin::Max:
            if (!expectArgs(2) || !ok || !expectNumeric()) return;
            node->type = unify(args[0], args[1]);
            return;

        case Builtin::Push:
            noteMutatingBuiltin(node, name);
            node->type = SemType::Void();
            if (!expectArgs(2) || !ok || !expectList()) return;
            coerce(args[1], args[0]->type.elementType());
            return;

        case Builtin::Pop:
            noteMutatingBuiltin(node, name);
            if (!expectArgs(1) || !ok || !expectList()) return;
            node->type = args[0]->type.elementType();
            return;

        // free(liste). String için de yazılabilir ama bir şey yapmaz: stringlerin
        // belleği kendiliğinden bırakılır.
        case Builtin::Free:
            node->type = SemType::Void();
            if (args.size() != 1 || !args[0]->type.isString()) noteMutatingBuiltin(node, name);
            if (!expectArgs(1) || !ok) return;
            if (!args[0]->type.isList() && !args[0]->type.isString())
                error(args[0].get(), "'free' liste ya da string bekler (bulunan: " +
                                     args[0]->type.str() + ")");
            return;

        // str(x): sayı, bool ya da char → print'in yazdığı metin.
        case Builtin::Str:
            if (!expectArgs(1) || !ok) return;
            if (!args[0]->type.isScalar() && !args[0]->type.isString()) {
                error(args[0].get(), "'str' sayisal, bool, char ya da string bekler (bulunan: " +
                                     args[0]->type.str() + ")");
                return;
            }
            node->type = SemType::String();
            return;

        // to_int(s) → int64, to_float(s) → float64. Metin sayı değilse program
        // çalışırken mesajla durur.
        case Builtin::ToInt: case Builtin::ToFloat:
            if (!expectArgs(1) || !ok) return;
            if (!args[0]->type.isString()) {
                error(args[0].get(), "'" + name + "' string bekler (bulunan: " +
                                     args[0]->type.str() + ")");
                return;
            }
            node->type = node->builtin == Builtin::ToInt ? SemType::Int(64) : SemType::Float(64);
            return;

        case Builtin::Await:
            if (!expectArgs(1) || !ok) return;
            checkAwait(node);
            return;

        case Builtin::Scan:
            if (!expectArgs(1) || !ok) return;
            checkScan(node, expected);
            return;

        case Builtin::Len: {
            if (!expectArgs(1) || !ok) return;
            const SemType& t = args[0]->type;
            if (!t.isList() && !t.isArray() && !t.isString()) {
                error(args[0].get(), "'len' liste, dizi ya da string bekler (bulunan: " + t.str() + ")");
                return;
            }
            node->type = SemType::Int(64);
            return;
        }
    }
}

void Sema::visit(FunctionCallExpr* node) {
    const SemType expected = takeHint();
    node->type = SemType::Error();

    // İsim kullanıcı tarafından tanımlanmamışsa bir struct (Nokta(1, 2)) ya da
    // yerleşik olabilir.
    if (auto* ident = dynamic_cast<Identifier*>(node->target.get())) {
        if (!lookup(ident)) {
            auto found = structs.find(linkNameOf(ident->name));
            if (found != structs.end()) {
                checkConstructor(node, ident, found->second);
                return;
            }
            node->builtin = lookupBuiltin(ident->name);
            if (node->builtin != Builtin::None) {
                checkBuiltinCall(node, ident->name, expected);
                return;
            }
        }
    }

    identIsCallee = dynamic_cast<Identifier*>(node->target.get()) != nullptr;
    SemType callee = check(node->target);
    identIsCallee = false;
    const bool callable = callee.isFunction() && node->arguments.size() == callee.paramCount();

    // Argümanlar, biliniyorsa parametre tipi beklentisiyle denetlenir.
    auto* calleeIdent = dynamic_cast<Identifier*>(node->target.get());
    const bool direct = calleeIdent && calleeIdent->ref == Identifier::Ref::Function;
    for (size_t i = 0; i < node->arguments.size(); ++i) {
        // Liste parametresinin başka bir fonksiyona verilmesi (SemaBorrow.cpp).
        // Görev olarak başlatılan çağrının argümanları kopyalanır: ödünçtür.
        if (dynamic_cast<Identifier*>(node->arguments[i].get())) {
            if (node == startingTask) argumentContext = {"", -2};
            else if (direct) argumentContext = {calleeIdent->name, static_cast<int>(i)};
        }
        if (callable) checkExpecting(node->arguments[i], callee.paramType(i));
        else check(node->arguments[i]);
        argumentContext = {"", -1};
        if (auto* literal = dynamic_cast<ListLiteral*>(node->arguments[i].get());
            literal && direct && node != startingTask)
            literalArguments.push_back({literal, calleeIdent->name, static_cast<int>(i)});
    }

    if (callee.isError()) return;
    if (!callee.isFunction()) {
        error(node->target.get(), "cagrilan ifade fonksiyon degil (bulunan: " + callee.str() + ")");
        return;
    }
    if (!callable) {
        error(node, "arguman sayisi uyusmuyor: beklenen " + std::to_string(callee.paramCount()) +
                    ", verilen " + std::to_string(node->arguments.size()));
        return;
    }
    for (size_t i = 0; i < node->arguments.size(); ++i)
        coerce(node->arguments[i], callee.paramType(i));
    node->type = callee.returnType();
    noteCall(node);
    checkGpuCall(node);
}

void Sema::visit(UnaryExpr* node) {
    node->type = SemType::Error();
    SemType t = check(node->operand);
    if (t.isError()) return;

    if (node->op == UnaryExpr::Negate) {
        if (!t.isNumeric() && !t.isMatrix()) {
            error(node, "'-' isleci sayisal bir deger bekler (bulunan: " + t.str() + ")");
            return;
        }
        node->type = t;
    } else {
        if (!t.isBool()) {
            error(node, "'!' isleci bool bekler (bulunan: " + t.str() + ")");
            return;
        }
        node->type = SemType::Bool();
    }
}

// Matris içeren + - * /  : işlem ELEMAN ELEMAN yapılır (matris çarpımı için
// matmul kullanılır). Karşı taraf aynı boyutta bir matris ya da bir sayı
// (her elemana uygulanır) olabilir. Sonucun eleman tipi iki tarafın ortak
// tipidir; gerekirse matrisler eleman eleman dönüştürülür.
SemType Sema::checkMatrixBinary(BinaryExpr* node) {
    const SemType lt = node->left->type;
    const SemType rt = node->right->type;

    const bool arithmetic = node->op == BinaryExpr::Add || node->op == BinaryExpr::Sub ||
                            node->op == BinaryExpr::Mul || node->op == BinaryExpr::Div;
    const SemType le = lt.isMatrix() ? lt.elementType() : lt;
    const SemType re = rt.isMatrix() ? rt.elementType() : rt;
    if (!arithmetic || !le.isNumeric() || !re.isNumeric()) {
        error(node, "matrislerle yalnizca + - * / kullanilabilir; karsi taraf matris ya da sayi "
                    "olmali (bulunan: " + lt.str() + " ve " + rt.str() + ")");
        return SemType::Error();
    }

    // İki matris aynı boyut sayısında olmalı (farklı boyut sayıları arasında
    // işlem henüz yok). Boyutların uzunlukları ise çalışırken denetlenir.
    if (lt.isMatrix() && rt.isMatrix() && lt.rank != rt.rank) {
        error(node, "matrislerin boyut sayilari farkli: " + lt.str() + " ve " + rt.str());
        return SemType::Error();
    }
    const int rank = lt.isMatrix() ? lt.rank : rt.rank;

    // Sayı literali matrisin eleman tipine uyar (m * 2 → 2 de float olur);
    // kayan nokta literali tamsayı matrisle karşılaşırsa ortak tip kayan nokta.
    auto adapts = [](const ExprPtr& scalar, const SemType& scalarType, const SemType& matrixElem) {
        return isLiteralLike(scalar.get()) && (scalarType.isInt() || matrixElem.isFloat());
    };
    SemType elem;
    if (!lt.isMatrix() && adapts(node->left, lt, re)) elem = re;
    else if (!rt.isMatrix() && adapts(node->right, rt, le)) elem = le;
    else elem = commonType(le, re);

    const SemType matrixType = SemType::Matrix(elem, rank);
    coerce(node->left, lt.isMatrix() ? matrixType : elem);
    coerce(node->right, rt.isMatrix() ? matrixType : elem);
    return matrixType;
}

void Sema::visit(BinaryExpr* node) {
    node->type = SemType::Error();
    SemType lt = check(node->left);
    SemType rt = check(node->right);
    if (lt.isError() || rt.isError()) return;

    auto mismatch = [&](const char* expected) {
        error(node, std::string("islenenler ") + expected + " olmali (bulunan: " +
                    lt.str() + " ve " + rt.str() + ")");
    };
    bool bothNumeric = lt.isNumeric() && rt.isNumeric();

    if (lt.isMatrix() || rt.isMatrix()) {
        node->type = checkMatrixBinary(node);
        return;
    }

    // Stringler: + birleştirir (yeni bir string üretir), karşılaştırmalar
    // sözlük sırasına göredir.
    if (lt.isString() || rt.isString()) {
        const bool comparison = node->op == BinaryExpr::Eq || node->op == BinaryExpr::Neq ||
                                node->op == BinaryExpr::Lt || node->op == BinaryExpr::Lte ||
                                node->op == BinaryExpr::Gt || node->op == BinaryExpr::Gte;
        if (!(lt.isString() && rt.isString()))
            error(node, "string yalnizca string ile kullanilabilir (bulunan: " + lt.str() + " ve " +
                        rt.str() + "); diger degerleri str(...) ile cevirin");
        else if (node->op == BinaryExpr::Add)
            node->type = SemType::String();
        else if (comparison)
            node->type = SemType::Bool();
        else
            error(node, "stringlerle yalnizca + ve karsilastirma (== != < <= > >=) kullanilabilir");
        return;
    }

    switch (node->op) {
        case BinaryExpr::Add: case BinaryExpr::Sub: case BinaryExpr::Mul:
        case BinaryExpr::Div: case BinaryExpr::Mod:
            if (!bothNumeric) return mismatch("sayisal");
            node->type = unify(node->left, node->right);
            return;

        case BinaryExpr::Lt: case BinaryExpr::Lte: case BinaryExpr::Gt: case BinaryExpr::Gte:
            if (bothNumeric) unify(node->left, node->right);
            else if (!(lt.isChar() && rt.isChar())) return mismatch("sayisal ya da char");
            node->type = SemType::Bool();
            return;

        case BinaryExpr::Eq: case BinaryExpr::Neq:
            if (bothNumeric) unify(node->left, node->right);
            else if (!(lt == rt && (lt.isBool() || lt.isChar())))
                return mismatch("ayni tipte sayisal, bool ya da char");
            node->type = SemType::Bool();
            return;

        case BinaryExpr::And: case BinaryExpr::Or:
            if (!lt.isBool() || !rt.isBool()) return mismatch("bool");
            node->type = SemType::Bool();
            return;
    }
}

// ===========================================================================
// Deyimler
// ===========================================================================

void Sema::visit(BlockStmt* node) {
    scopes.emplace_back();
    for (auto& stmt : node->statements) stmt->accept(*this);
    scopes.pop_back();
}

void Sema::visit(ExprStmt* node) { check(node->expr); }

// Yerel değişken tanımı (globaller checkGlobal'da işlenir).
void Sema::visit(VariableDecl* node) {
    SemType type;
    if (node->type) type = resolveValueType(node->type.get());

    if (node->initializer) {
        SemType initType = node->type ? checkExpecting(node->initializer, type)
                                      : check(node->initializer);
        if (node->type) {
            coerce(node->initializer, type);
        } else if (initType.isVoid()) {
            error(node->initializer.get(), "void donduren bir ifade degiskene atanamaz");
        } else {
            type = initType; // let: tip başlangıç değerinden çıkarılır
        }
    } else if (type.isFunction()) {
        error(node, "fonksiyon tipli degisken baslangic degeri olmadan tanimlanamaz");
        type = SemType::Error();
    } else if (type.isTask()) {
        error(node, "task degiskeni baslangic degeri olmadan tanimlanamaz (or. " + type.str() +
                    " t = marl f(...);)");
        type = SemType::Error();
    } else if (type.isMatrix()) {
        error(node, "matris baslangic degeri olmadan tanimlanamaz (or. zeros(3, 4))");
        type = SemType::Error();
    } else if (type.containsMatrix()) {
        error(node, "matris alani olan struct baslangic degeri olmadan tanimlanamaz (or. " +
                    type.str() + "(...))");
        type = SemType::Error();
    } else if (type.containsFunction()) {
        error(node, "fonksiyon alani olan struct baslangic degeri olmadan tanimlanamaz (or. " +
                    type.str() + "(...))");
        type = SemType::Error();
    }

    if (gpuFunction) {
        checkGpuType(type, node);
        if (type.isMatrix() && inGpuKernel())
            error(node, "GPU'da calisan paralel dongunun icinde matris tanimlanamaz");
    }

    node->varType = type;
    declareVariable(node->name, {type, Symbol::Variable, funcDepth}, node);
}

void Sema::visit(AssignmentStmt* node) {
    // Hedef okunmuyor, yazılıyor: paralel döngü denetimi için işaretle.
    auto* targetAccess = dynamic_cast<ArrayAccessExpr*>(node->target.get());
    auto* targetMember = dynamic_cast<MemberExpr*>(node->target.get());
    if (targetAccess) accessIsWrite = true;
    else if (!targetMember) skipWholeRead = true;
    SemType target = check(node->target);
    accessIsWrite = skipWholeRead = false;
    SemType value = checkExpecting(node->value, target);

    // l[i].x = ... — alanına atanan struct bir liste elemanı olmalı. (Değişkenin
    // alanı `p.x = ...` noktalı isim olarak gelir; f().x ise geçici bir değerdir.)
    if (targetMember && !target.isError()) {
        Expression* base = targetMember->base.get();
        while (auto* inner = dynamic_cast<MemberExpr*>(base)) base = inner->base.get();
        auto* element = dynamic_cast<ArrayAccessExpr*>(base);
        if (!element || !element->target->type.isList())
            return error(node->target.get(), "'." + targetMember->field + "' alanina atama yapilamaz: "
                         "hedef bir degiskenin (p.x) ya da liste elemaninin (l[i].x) alani olmali");
    }
    // Hedef, bir değişkenin doğrudan elemanı değilse (l[i].x, l[i].dizi[j])
    // paralel döngü denetimi için ayrıca kaydedilir.
    if (!target.isError() &&
        (targetMember || (targetAccess && !dynamic_cast<Identifier*>(targetAccess->target.get()))))
        noteNestedWrite(node->target.get());

    if (auto* ident = dynamic_cast<Identifier*>(node->target.get())) {
        if (Symbol* sym = lookup(ident); sym && !target.isError()) {
            if (sym->kind == Symbol::Function)
                return error(ident, "'" + ident->name + "' bir fonksiyon; atama yapilamaz");
            if (sym->kind == Symbol::Iterator)
                return error(ident, "dongu degiskeni '" + ident->name + "' degistirilemez");
        }
    }

    // Matris parametreleri salt okunurdur: çağıranın matrisi kopyalanmadan
    // verilir, bu yüzden fonksiyon onu değiştiremez (değer semantiği).
    Expression* root = node->target.get();
    while (true) {
        if (auto* access = dynamic_cast<ArrayAccessExpr*>(root)) root = access->target.get();
        else if (auto* member = dynamic_cast<MemberExpr*>(root)) root = member->base.get();
        else break;
    }
    if (auto* ident = dynamic_cast<Identifier*>(root)) {
        Symbol* sym = lookup(ident);
        // `for p in liste` değişkeninin dizi ve matris alanları listedeki
        // elemanın kendisidir; döngü değişkeni üzerinden değiştirilemez.
        if (sym && sym->kind == Symbol::Iterator && root != node->target.get() &&
            (sym->type.isArray() || sym->type.isMatrix()))
            return error(ident, "dongu degiskeni '" + ident->name + "' degistirilemez; listedeki "
                                "elemani indeksle degistirin (or. liste[i]." +
                                ident->name.substr(ident->name.rfind('.') + 1) + ")");
        if (sym && sym->readOnly && sym->type.isStruct())
            return error(ident, "'" + ident->name + "' parametresi matris iceriyor; butun olarak "
                                "atanamaz (matris parametreleri salt okunurdur)");
        if (sym && sym->readOnly)
            return error(ident, "matris parametresi '" + ident->name + "' degistirilemez; yerel bir "
                                "kopya kullanin (or. matrix<float> m = " + ident->name + ";)");
    }
    if (auto* ident = dynamic_cast<Identifier*>(node->target.get()))
        if (Symbol* sym = lookup(ident); sym && !target.isError())
            noteWholeWrite(ident, *sym);

    if (value.isVoid())
        return error(node->value.get(), "void donduren bir ifade atanamaz");
    coerce(node->value, target);
}

void Sema::visit(IfStmt* node) {
    checkCondition(node->condition, "if");

    flow.push_back({false});
    node->trueBlock->accept(*this);
    for (auto& elseIf : node->elseIfs) {
        checkCondition(elseIf.condition, "else if");
        elseIf.block->accept(*this);
    }
    if (node->elseBlock) node->elseBlock->accept(*this);
    FlowFrame frame = flow.back();
    flow.pop_back();

    node->needsCFG = frame.sawBreak || frame.sawContinue || frame.sawReturn;
}

void Sema::visitLoopBody(LoopStmt* loop) {
    flow.push_back({true});
    loop->body->accept(*this);
    FlowFrame frame = flow.back();
    flow.pop_back();

    loop->needsCFG = frame.sawBreak || frame.sawReturn;
    loop->hasContinue = frame.sawContinue;
}

void Sema::visit(WhileStmt* node) {
    checkCondition(node->condition, "while");
    visitLoopBody(node);
}

void Sema::visit(ForRangeStmt* node) {
    std::vector<ExprPtr*> parts = {&node->start, &node->end};
    if (node->step) parts.push_back(&node->step);

    bool ok = true;
    for (ExprPtr* p : parts) {
        SemType t = check(*p);
        if (t.isError()) { ok = false; continue; }
        if (!t.isInt()) {
            error(p->get(), "for araligi tamsayi olmali (bulunan: " + t.str() + ")");
            ok = false;
        }
    }
    node->iterType = ok ? unifyAll(parts) : SemType::Error();

    // Adım pozitif olmak zorunda: hem scf.for hem de `i < end` karşılaştırması
    // bunu varsayar. Sabit adımlar burada denetlenir.
    if (node->step) {
        auto* lit = dynamic_cast<IntegerLiteral*>(node->step.get());
        bool negated = dynamic_cast<UnaryExpr*>(node->step.get()) && isLiteralLike(node->step.get());
        if (negated || (lit && lit->value <= 0))
            error(node->step.get(), "step pozitif olmali (azalan araliklar henuz desteklenmiyor)");
    }

    scopes.emplace_back();
    declare(node->iteratorName, {node->iterType, Symbol::Iterator, funcDepth}, node);

    if (node->isParallel) checkGpuParallelFor(node);
    // CPU'daki paralel döngü, yinelemeleri iş parçacıklarına dağıtır ve hepsinin
    // bitmesini bekler (görevlerin renklendirilmesi bunu bilmelidir).
    if (node->isParallel && !gpuFunction && currentEffects &&
        currentEffects->parallelReason.empty())
        currentEffects->parallelReason = "paralel dongu iceriyor";
    if (node->isParallel)
        parallelLoops.push_back({node->iteratorName, static_cast<int>(scopes.size()) - 1, {}, {}});
    visitLoopBody(node);
    if (node->isParallel) {
        // Yinelemeler aynı anda çalıştığı için "döngüden çık" anlamsızdır.
        if (node->needsCFG)
            error(node, "paralel dongunun icinden break ya da return ile cikilamaz");
        ParallelLoop loop = std::move(parallelLoops.back());
        parallelLoops.pop_back();
        finishParallelLoop(loop);
    }
    scopes.pop_back();
}

void Sema::visit(ForCollectionStmt* node) {
    if (node->isParallel)
        error(node, "'parallel' yalnizca aralik dongusunde kullanilabilir "
                    "(or. parallel for i in 0 .. len(liste) { ... })");
    identIsIterated = dynamic_cast<Identifier*>(node->collection.get()) != nullptr;
    SemType coll = check(node->collection);
    identIsIterated = false;
    node->iterType = SemType::Error();
    if (!coll.isError()) {
        if (coll.isArray() || coll.isList()) node->iterType = coll.elementType();
        else error(node->collection.get(), "for ... in bir dizi ya da liste bekler (bulunan: " + coll.str() + ")");
    }

    scopes.emplace_back();
    declareVariable(node->iteratorName, {node->iterType, Symbol::Iterator, funcDepth}, node);
    visitLoopBody(node);
    scopes.pop_back();
}

void Sema::visit(BreakStmt* node) {
    for (size_t i = flow.size(); i-- > 0;) {
        flow[i].sawBreak = true;
        if (flow[i].isLoop) return;
    }
    error(node, "'break' bir dongunun icinde olmali");
}

void Sema::visit(ContinueStmt* node) {
    for (size_t i = flow.size(); i-- > 0;) {
        flow[i].sawContinue = true;
        if (flow[i].isLoop) return;
    }
    error(node, "'continue' bir dongunun icinde olmali");
}

void Sema::visit(ReturnStmt* node) {
    for (auto& frame : flow) frame.sawReturn = true;

    const SemType ret = returnTypes.back();
    if (node->expr) {
        SemType t = checkExpecting(node->expr, ret);
        if (ret.isVoid()) {
            if (!t.isError()) error(node, "void fonksiyon deger donduremez");
        } else {
            coerce(node->expr, ret);
        }
    } else if (!ret.isVoid() && !ret.isError()) {
        error(node, "return bir deger bekliyor (" + ret.str() + ")");
    }
}

// Deyim her yolda return ile mi bitiyor? (Döngüler sayılmaz: gövde hiç
// çalışmayabilir.)
bool Sema::alwaysReturns(const Statement* s) {
    if (dynamic_cast<const ReturnStmt*>(s)) return true;
    if (auto* block = dynamic_cast<const BlockStmt*>(s)) {
        for (auto& stmt : block->statements)
            if (alwaysReturns(stmt.get())) return true;
        return false;
    }
    if (auto* ifStmt = dynamic_cast<const IfStmt*>(s)) {
        if (!ifStmt->elseBlock || !alwaysReturns(ifStmt->trueBlock.get())) return false;
        for (auto& elseIf : ifStmt->elseIfs)
            if (!alwaysReturns(elseIf.block.get())) return false;
        return alwaysReturns(ifStmt->elseBlock.get());
    }
    return false;
}
