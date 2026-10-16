// Sema — görevler: marl, task<T>, await (kurallar ve renklendirme: Sema.h)

#include "Sema.h"

#include <cstdint>
#include <unordered_set>

std::string Sema::lambdaKey(const LambdaExpr* lambda) {
    return "lambda@" + std::to_string(reinterpret_cast<std::uintptr_t>(lambda));
}

// Görevin sonucu: tek bir değer olarak saklanabilen tipler. Struct, matris ve
// iç içe yapılar henüz yok.
bool Sema::taskResultAllowed(const SemType& type) {
    auto single = [](const SemType& t) {
        return t.isScalar() || t.isString() || t.isFunction();
    };
    if (type.isVoid() || single(type)) return true;
    return type.isList() && single(type.elementType());
}

// Görevin argümanı: görev başlamadan kopyalanabilen tipler. Struct, alanlarına
// tek tek uygulanır. Liste elemanları kopyalanırken yalnızca tek değerlik
// elemanlar desteklenir (listenin içindeki liste, dizi ya da matris henüz yok).
bool Sema::taskArgumentAllowed(const SemType& type) {
    for (const SemLeaf& leaf : leavesOf(type)) {
        const SemType& t = leaf.type;
        if (t.isList()) {
            const SemType& elem = t.elementType();
            if (!(elem.isScalar() || elem.isString() || elem.isFunction() || elem.isTask()))
                return false;
        } else if (!(t.isScalar() || t.isString() || t.isFunction() || t.isTask() ||
                     t.isArray() || t.isMatrix())) {
            return false;
        }
    }
    return true;
}

void Sema::noteGlobalWrite(const std::string& name) {
    if (currentEffects && currentEffects->globalWriteReason.empty())
        currentEffects->globalWriteReason = "global degisken '" + name + "' uzerine yaziyor";
}

void Sema::noteValueFunction(const std::string& name, const SemType& type, const ASTNode* at,
                             bool isLambda) {
    valueFunctions.push_back({name, type, at, isLambda, currentFunction});
}

// marl f(a, b)
void Sema::visit(MarlExpr* node) {
    node->type = SemType::Error();
    auto* call = dynamic_cast<FunctionCallExpr*>(node->call.get());
    if (!call) {
        check(node->call);
        error(node, "'marl' bir fonksiyon cagrisi bekler (or. marl hesapla(x))");
        return;
    }
    if (gpuFunction) error(node, "@GPU fonksiyonda marl kullanilamaz");
    if (inParallel()) error(node, "paralel dongude marl kullanilamaz");

    // Çağrı sıradan bir çağrı gibi denetlenir; noteCall onu bu fonksiyonun
    // çağrısı değil, başlatılan bir görev olarak kaydeder.
    FunctionCallExpr* outerTask = startingTask;
    startingTask = call;
    check(node->call);
    startingTask = outerTask;

    if (call->builtin != Builtin::None) {
        error(node, "'marl' yerlesik bir fonksiyonla kullanilamaz; onu cagiran bir fonksiyon "
                    "yazip o fonksiyonu baslatin");
        return;
    }
    if (call->isConstructor) {
        error(node, "'marl' bir fonksiyon cagrisi bekler (struct olusturma degil)");
        return;
    }
    if (call->type.isError()) return;

    if (auto* ident = dynamic_cast<Identifier*>(call->target.get());
        ident && ident->ref == Identifier::Ref::Function) {
        auto found = functionDecls.find(ident->name);
        if (found != functionDecls.end() && found->second->isExtern) {
            error(node, "extern fonksiyon dogrudan marl ile baslatilamaz");
            return;
        }
        if (ident->name == "main") {
            error(node, "'main' gorev olarak baslatilamaz");
            return;
        }
    }

    bool ok = true;
    if (!taskResultAllowed(call->type)) {
        error(node, "sonucu " + call->type.str() + " olan fonksiyon henuz gorev olarak "
                    "baslatilamaz (sonuc sayi, bool, char, string, fonksiyon, void ya da "
                    "bunlarin listesi olabilir)");
        ok = false;
    }
    for (auto& arg : call->arguments) {
        if (arg->type.isError() || taskArgumentAllowed(arg->type)) continue;
        error(arg.get(), "gorev argumani olarak " + arg->type.str() + " henuz desteklenmiyor "
                         "(elemani liste, dizi ya da matris olan listeler kopyalanamiyor)");
        ok = false;
    }
    if (ok) node->type = SemType::Task(call->type);
}

// await(t)
void Sema::checkAwait(FunctionCallExpr* node) {
    auto& arg = node->arguments[0];
    if (!arg->type.isTask()) {
        error(arg.get(), "'await' bir gorev (task) bekler (bulunan: " + arg->type.str() + ")");
        return;
    }
    if (gpuFunction) error(node, "@GPU fonksiyonda await kullanilamaz");
    if (inParallel())
        error(node, "paralel dongude await kullanilamaz (yinelemeler birbirini beklememeli)");
    if (currentEffects && currentEffects->awaitReason.empty())
        currentEffects->awaitReason = "await kullaniyor";
    // Bekleyen fonksiyon paralel döngüden de çağrılamaz.
    noteUnsafe("await kullaniyor");
    node->type = arg->type.elementType();
}

// Program bitti: bütün fonksiyonların etkileri biliniyor.
void Sema::checkTasks() {
    // 1) Etkiler çağrı zinciri boyunca yayılır: f, g'yi çağırıyorsa ve g
    //    global'e yazıyorsa f de yazıyordur.
    auto propagate = [&](std::string FunctionEffects::*reason) {
        for (bool changed = true; changed;) {
            changed = false;
            for (auto& [name, fn] : effects) {
                if (!(fn.*reason).empty()) continue;
                for (auto& callee : fn.callees) {
                    auto found = effects.find(callee);
                    if (found == effects.end() || (found->second.*reason).empty()) continue;
                    fn.*reason = "'" + callee + "' fonksiyonunu cagiriyor; o da " + found->second.*reason;
                    changed = true;
                    break;
                }
            }
        }
    };
    propagate(&FunctionEffects::globalWriteReason);
    propagate(&FunctionEffects::awaitReason);
    propagate(&FunctionEffects::parallelReason);

    auto effectsOf = [&](const std::string& name) -> const FunctionEffects* {
        auto found = effects.find(name);
        return found == effects.end() ? nullptr : &found->second;
    };
    auto describe = [](const ValueFunction& v) {
        return v.isLambda ? "satir " + std::to_string(v.at->line) + "'deki lambda"
                          : "'" + v.name + "'";
    };

    // 2) Kural 4: await içeren fonksiyon değer olarak kullanılamaz (dolaylı
    //    çağrı coroutine'i bekleyemez).
    for (auto& v : valueFunctions) {
        const FunctionEffects* fx = effectsOf(v.name);
        if (fx && !fx->awaitReason.empty())
            error(v.at, describe(v) + " deger olarak kullanilamaz: " + fx->awaitReason +
                        " (await iceren fonksiyon yalnizca adiyla cagrilabilir)");
    }
    // 3) Görev kuralları yalnızca çalışabilecek kodda uygulanır: main'den doğrudan çağrılarla,
    //    değer olarak kullanılarak ya da görev olarak başlatılarak ulaşılan
    //    fonksiyonlar. (Böylece kullanılmayan bir kütüphane fonksiyonundaki
    //    marl, programın geri kalanına kural koymaz.)
    std::unordered_set<std::string> live;
    {
        std::vector<std::string> pending = {"main"};
        while (!pending.empty()) {
            std::string name = std::move(pending.back());
            pending.pop_back();
            if (!live.insert(name).second) continue;
            if (const FunctionEffects* fx = effectsOf(name))
                for (auto& callee : fx->callees) pending.push_back(callee);
            for (auto& v : valueFunctions)
                if (v.owner == name) pending.push_back(v.name);
            for (auto& start : taskStarts)
                if (start.owner == name && !start.callee.empty()) pending.push_back(start.callee);
        }
    }
    std::vector<const TaskStart*> starts;
    for (auto& start : taskStarts)
        if (live.count(start.owner)) starts.push_back(&start);
    std::vector<const ValueFunction*> values;
    for (auto& v : valueFunctions)
        if (live.count(v.owner)) values.push_back(&v);
    if (starts.empty()) return;
    const_cast<Program*>(program)->startsTasks = true;

    // 4) Görevden ulaşılan fonksiyonlar: başlatılanlar, onların doğrudan
    //    çağırdıkları ve dolaylı çağrılarda gidilebilecek (aynı tipte, değer
    //    olarak kullanılmış) fonksiyonlar.
    std::unordered_set<std::string> reachable;
    std::vector<std::string> work;
    auto addCandidates = [&](const SemType& type) {
        for (const ValueFunction* v : values)
            if (v->type == type) work.push_back(v->name);
    };
    for (const TaskStart* startPtr : starts) {
        const TaskStart& start = *startPtr;
        if (start.callee == "main") continue; // `marl main()` zaten raporlandı
        if (!start.callee.empty()) work.push_back(start.callee);
        else addCandidates(start.valueType);
    }
    while (!work.empty()) {
        std::string name = std::move(work.back());
        work.pop_back();
        if (!reachable.insert(name).second) continue;
        const FunctionEffects* fx = effectsOf(name);
        if (!fx) continue;
        for (auto& callee : fx->callees) work.push_back(callee);
        for (auto& call : fx->indirectCalls) addCandidates(call.type);
    }

    // 5) Kural 2: görev olarak başlatılan fonksiyon global'e yazamaz.
    //    Fonksiyon değeriyle başlatılan görevde, değerin gösterebileceği her
    //    fonksiyon denetlenir; onların paralel döngüsü de olamaz (coroutine
    //    olmadıkları için beklerken iş parçacığını tutarlar).
    for (const TaskStart* start : starts) {
        if (!start->callee.empty()) {
            const FunctionEffects* fx = effectsOf(start->callee);
            if (fx && !fx->globalWriteReason.empty())
                error(start->at, "'" + start->callee + "' gorev olarak baslatilamaz: " +
                                 fx->globalWriteReason + " (gorevler global degiskene yazamaz; "
                                 "sonucu await ile alin)");
            continue;
        }
        for (const ValueFunction* v : values) {
            if (v->type != start->valueType) continue;
            const FunctionEffects* fx = effectsOf(v->name);
            if (!fx) continue;
            if (!fx->globalWriteReason.empty())
                error(start->at, "bu fonksiyon degeri gorev olarak baslatilamaz: gosterebilecegi " +
                                 describe(*v) + " " + fx->globalWriteReason);
            else if (!fx->parallelReason.empty())
                error(start->at, "bu fonksiyon degeri gorev olarak baslatilamaz: gosterebilecegi " +
                                 describe(*v) + " " + fx->parallelReason +
                                 " (adiyla baslatin: marl " + v->name + "(...))");
        }
    }

    // 6) Kural 3: görevden ulaşılan kodda dolaylı çağrılar.
    for (const std::string& name : reachable) {
        const FunctionEffects* fx = effectsOf(name);
        if (!fx) continue;
        for (auto& call : fx->indirectCalls) {
            for (const ValueFunction* v : values) {
                if (v->type != call.type) continue;
                const FunctionEffects* target = effectsOf(v->name);
                if (!target) continue;
                if (!target->globalWriteReason.empty())
                    error(call.at, "gorevden ulasilan bu cagri " + describe(*v) +
                                   " fonksiyonuna gidebilir; o " + target->globalWriteReason +
                                   " (gorevler global degiskene yazamaz)");
                else if (!target->parallelReason.empty())
                    error(call.at, "gorevden ulasilan bu cagri " + describe(*v) +
                                   " fonksiyonuna gidebilir; o " + target->parallelReason +
                                   " (gorev icinde fonksiyon degeriyle cagrilamaz; adiyla cagirin)");
            }
        }
    }

    // 7) Renklendirme: görevden ulaşılan ve bekleyebilen fonksiyonlar
    //    coroutine olur. (Değer olarak kullanılması, ölü kodda bile olsa
    //    raporlanır: coroutine'in adresi alınamaz.)
    for (const std::string& name : reachable) {
        auto decl = functionDecls.find(name);
        if (decl == functionDecls.end()) continue; // lambda
        FunctionDecl* fn = decl->second;
        const FunctionEffects* fx = effectsOf(name);
        if (fn->isExtern || fn->isGpu || !fx) continue;
        if (fx->awaitReason.empty() && fx->parallelReason.empty()) continue;

        fn->isAsync = true;
        const std::string& why = fx->awaitReason.empty() ? fx->parallelReason : fx->awaitReason;
        if (name == "main")
            error(fn, "'main' bir gorevden cagrilamaz");
        if (fn->fnType.returnType().isComposite())
            error(fn, "gorevden cagrilan ve bekleyen '" + name + "' (" + why +
                      ") henuz struct donduremez");
        // await içerenler 2. adımda raporlandı; burada paralel döngü içerenler.
        if (fx->awaitReason.empty())
            for (auto& v : valueFunctions)
                if (v.name == name)
                    error(v.at, "'" + name + "' deger olarak kullanilamaz: gorevden cagriliyor ve " +
                                why + " (yalnizca adiyla cagrilabilir)");
    }

    // Paralel döngünün gövdesi coroutine değildir: coroutine olan bir
    // fonksiyonu oradan çağırmak, beklerken iş parçacığını tutardı.
    for (auto& call : parallelCalls) {
        auto decl = functionDecls.find(call.callee);
        if (decl != functionDecls.end() && decl->second->isAsync)
            error(call.at, "paralel dongude '" + call.callee + "' cagrilamaz: gorevlerden de "
                           "cagriliyor ve bekleyebilen bir fonksiyon");
    }
}
