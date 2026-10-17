// Sema — paralel döngü güvenliği (kurallar: Sema.h)

#include "Sema.h"

#include <map>

bool Sema::isOutside(const Symbol& sym, const ParallelLoop& loop) {
    return sym.kind == Symbol::Global || sym.scopeDepth < loop.bodyDepth;
}

// İndekslerden hangileri tam olarak bu döngünün değişkeni? (i + 1 ya da aynı
// adlı başka bir değişken sayılmaz.)
unsigned Sema::iteratorMask(const ArrayAccessExpr* access, const ParallelLoop& loop) {
    unsigned mask = 0;
    for (size_t i = 0; i < access->indices.size(); ++i) {
        auto* ident = dynamic_cast<Identifier*>(access->indices[i].get());
        if (!ident || ident->name != loop.iterator) continue;
        Symbol* sym = lookup(ident);
        if (sym && sym->kind == Symbol::Iterator && sym->scopeDepth == loop.bodyDepth)
            mask |= 1u << i;
    }
    return mask;
}

void Sema::noteUnsafe(const std::string& reason) {
    if (currentEffects && currentEffects->unsafeReason.empty())
        currentEffects->unsafeReason = reason;
}

// Dışarıdaki bir dizi/liste/matrisin bütün olarak kullanılması (fonksiyona
// verilmesi, sum(a), for x in a ...): hangi elemanlarının okunacağı bilinmez.
void Sema::noteWholeRead(Identifier* ident, const Symbol& sym) {
    if (!inParallel()) return;
    if (sym.kind != Symbol::Variable && sym.kind != Symbol::Global) return;
    // Struct'ın bütün olarak kullanılması, dizi/liste/matris alanlarının her
    // birinin bütün olarak kullanılmasıdır.
    if (sym.type.isStruct()) {
        for (const SemLeaf& leaf : leavesOf(sym.type, ident->name)) {
            if (!leaf.type.isArray() && !leaf.type.isList() && !leaf.type.isMatrix()) continue;
            for (auto& loop : parallelLoops)
                if (isOutside(sym, loop)) loop.reads.push_back({leaf.path, ident, 0});
        }
        return;
    }
    if (!sym.type.isArray() && !sym.type.isList() && !sym.type.isMatrix()) return;

    for (auto& loop : parallelLoops)
        if (isOutside(sym, loop))
            loop.reads.push_back({ident->name, ident, 0});
}

// degisken = ...
void Sema::noteWholeWrite(Identifier* ident, const Symbol& sym) {
    if (sym.kind == Symbol::Global) {
        noteUnsafe("global degisken '" + ident->name + "' uzerine yaziyor");
        noteGlobalWrite(ident->name);
    }

    for (auto& loop : parallelLoops) {
        if (!isOutside(sym, loop)) continue;
        error(ident, "paralel dongude dongu disinda tanimli '" + ident->name +
                     "' degiskenine atama yapilamaz (yinelemeler ayni anda calisir); yalnizca '" +
                     loop.iterator + "' ile indekslenen elemanlara yazilabilir");
        return;
    }
}

// a[...]  (okuma)  ya da  a[...] = ...  (yazma)
void Sema::noteElementAccess(ArrayAccessExpr* access, bool isWrite) {
    auto* root = dynamic_cast<Identifier*>(access->target.get());
    if (!root) return;
    Symbol* sym = lookup(root);
    if (!sym || (sym->kind != Symbol::Variable && sym->kind != Symbol::Global)) return;

    if (isWrite) {
        if (sym->kind == Symbol::Global) {
            noteUnsafe("global '" + root->name + "' uzerine yaziyor");
            noteGlobalWrite(root->name);
        }
        else if (sym->type.isList())
            noteUnsafe("'" + root->name + "' listesini degistiriyor");
        else if (sym->isParam && sym->type.isArray())
            noteUnsafe("parametre olarak aldigi '" + root->name + "' dizisini degistiriyor");
    }

    for (auto& loop : parallelLoops) {
        if (!isOutside(*sym, loop)) continue;
        const unsigned mask = iteratorMask(access, loop);
        if (isWrite && mask == 0) {
            error(access, "paralel dongude '" + root->name + "' elemanina yalnizca dongu degiskeni '" +
                          loop.iterator + "' ile indekslenerek yazilabilir (or. " + root->name + "[" +
                          loop.iterator + "] = ...); aksi halde iki yineleme ayni elemana yazabilir");
            return;
        }
        (isWrite ? loop.writes : loop.reads).push_back({root->name, access, mask});
    }
}

// l[i].x = ...,  l[i].dizi[j] = ...,  f()[i] = ...  — hedefi bir değişkenin
// doğrudan elemanı olmayan yazma. Kökteki değişkenin indekslendiği erişim
// (l[i]) bulunur ve yazma o elemana yapılmış sayılır: her yineleme farklı bir
// elemanın içine yazıyorsa yarış yoktur.
void Sema::noteNestedWrite(Expression* target) {
    ArrayAccessExpr* rootAccess = nullptr;
    int listLevels = 0;
    Expression* expr = target;
    while (true) {
        if (auto* member = dynamic_cast<MemberExpr*>(expr)) {
            expr = member->base.get();
            continue;
        }
        auto* access = dynamic_cast<ArrayAccessExpr*>(expr);
        if (!access) break;
        if (access->target->type.isList()) ++listLevels;
        rootAccess = access;
        expr = access->target.get();
    }
    if (!rootAccess) return;

    auto* root = dynamic_cast<Identifier*>(expr);
    Symbol* sym = root ? lookup(root) : nullptr;
    if (!sym || (sym->kind != Symbol::Variable && sym->kind != Symbol::Global)) {
        // Kök bir değişken değil (ör. fonksiyonun döndürdüğü liste): neye
        // yazıldığı bilinmez.
        noteUnsafe("bir ifadenin dondurdugu listeyi degistiriyor");
        if (inParallel())
            error(target, "paralel dongude yalnizca bir degiskenin elemanina yazilabilir");
        return;
    }

    // Elemanın içindeki liste bir başvurudur: iki eleman aynı listeyi
    // gösterebilir, bu yüzden farklı yinelemeler aynı belleğe yazabilir.
    if (listLevels > 1) {
        for (auto& loop : parallelLoops) {
            if (!isOutside(*sym, loop)) continue;
            error(target, "paralel dongude '" + root->name + "' elemanlarinin icindeki listelere "
                          "yazilamaz (iki eleman ayni listeyi paylasiyor olabilir)");
            return;
        }
    }
    noteElementAccess(rootAccess, /*isWrite=*/true);
}

// m[k] = ...,  m[k].x = ... — map'e yazma anahtar ekleyebilir (tablo ve
// listeler büyür); iki yineleme aynı anda yapamaz. Map paralel döngüde
// yalnızca okunur.
void Sema::noteMapWrite(const ASTNode* at) {
    noteUnsafe("bir map'i degistiriyor");
    if (inParallel())
        error(at, "paralel dongude map degistirilemez (yalnizca okunabilir)");
}

void Sema::noteMutatingBuiltin(const ASTNode* at, const std::string& name) {
    noteUnsafe("'" + name + "' ile bir listeyi degistiriyor");
    if (inParallel())
        error(at, "paralel dongude '" + name + "' kullanilamaz");
}

void Sema::noteCall(FunctionCallExpr* call) {
    auto* ident = dynamic_cast<Identifier*>(call->target.get());
    const bool direct = ident && ident->ref == Identifier::Ref::Function;

    // marl f(...): f bu fonksiyonun içinde çağrılmaz, ayrı bir görevde çalışır;
    // etkileri bu fonksiyona geçmez (bkz. SemaTask.cpp).
    if (call == startingTask) {
        taskStarts.push_back({direct ? ident->name : "", call->target->type, call, currentFunction});
        return;
    }

    if (!direct) {
        if (currentEffects) currentEffects->indirectCalls.push_back({call->target->type, call});
        // Hangi fonksiyonun çağrılacağı derlerken bilinmiyor.
        noteUnsafe("degisken uzerinden (dolayli) fonksiyon cagiriyor");
        if (inParallel())
            error(call, "paralel dongude yalnizca adiyla tanimli fonksiyonlar cagrilabilir "
                        "(lambda ve fonksiyon degiskeni cagrilamaz)");
        return;
    }
    if (currentEffects) currentEffects->callees.push_back(ident->name);
    if (inParallel()) parallelCalls.push_back({ident->name, call});
}

// Döngü bitti: yazılan her dizi için okumaları denetle (kural 3).
void Sema::finishParallelLoop(ParallelLoop& loop) {
    // Bir diziye birden çok yerden yazılıyorsa hepsinde döngü değişkeni aynı
    // konumda durmalı: a[i, 0] ve a[0, i] farklı yinelemelerde çakışabilir.
    std::map<std::string, unsigned> writeMasks;
    for (auto& write : loop.writes) {
        auto [it, inserted] = writeMasks.emplace(write.name, write.iteratorMask);
        if (inserted) continue;
        it->second &= write.iteratorMask;
        if (it->second == 0) {
            error(write.at, "paralel dongude '" + write.name + "' elemanlarina yazarken '" +
                            loop.iterator + "' hep ayni konumda indeks olmali");
            it->second = write.iteratorMask;
        }
    }

    for (auto& read : loop.reads) {
        auto written = writeMasks.find(read.name);
        if (written == writeMasks.end() || (read.iteratorMask & written->second)) continue;
        error(read.at, "'" + read.name + "' bu paralel dongude yaziliyor; ayni dongude yalnizca '" +
                       loop.iterator + "' ile (yazildigi konumda) indekslenerek okunabilir. "
                       "Baska bir yinelemenin yazdigi eleman okunamaz");
    }
}

// Program bitti: bütün fonksiyonların etkileri biliniyor. Güvensizlik çağrı
// zinciri boyunca yayılır (f, g'yi çağırıyorsa ve g güvensizse f de güvensizdir).
void Sema::checkParallelCalls() {
    bool changed = true;
    while (changed) {
        changed = false;
        for (auto& [name, fn] : effects) {
            if (!fn.unsafeReason.empty()) continue;
            for (auto& callee : fn.callees) {
                auto found = effects.find(callee);
                if (found == effects.end() || found->second.unsafeReason.empty()) continue;
                fn.unsafeReason = "'" + callee + "' fonksiyonunu cagiriyor; o da " +
                                  found->second.unsafeReason;
                changed = true;
                break;
            }
        }
    }

    // GPU'ya is vermek de ayni sekilde yayilir (bkz. @GPU kurallari, kural 5).
    changed = true;
    while (changed) {
        changed = false;
        for (auto& [name, fn] : effects) {
            if (!fn.gpuWorkReason.empty()) continue;
            for (auto& callee : fn.callees) {
                auto found = effects.find(callee);
                if (found == effects.end() || found->second.gpuWorkReason.empty()) continue;
                fn.gpuWorkReason = "'" + callee + "' fonksiyonunu cagiriyor; o da " +
                                   found->second.gpuWorkReason;
                changed = true;
                break;
            }
        }
    }

    for (auto& call : parallelCalls) {
        auto found = effects.find(call.callee);
        if (found == effects.end()) continue;
        if (!found->second.unsafeReason.empty())
            error(call.at, "paralel dongude '" + call.callee + "' cagrilamaz: " +
                           found->second.unsafeReason);
        else if (!found->second.gpuWorkReason.empty())
            error(call.at, "paralel dongude '" + call.callee + "' cagrilamaz: " +
                           found->second.gpuWorkReason +
                           " (paralel dongunun icinden GPU'ya yeni is verilemez)");
    }
}
