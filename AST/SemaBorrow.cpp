// Sema — fonksiyona doğrudan verilen liste literallerinin bırakılması
// (kurallar: Sema.h, "Liste literallerinin bırakılması")
//
//   query(db, "...", [arg_int(1), arg_text("a")])
//
// Literalin adı yoktur; çağıran onu free edemez. query'nin `params`
// parametresi yalnızca ödünç alınıyorsa (üzerinde dönülüyor, indeksleniyor,
// başka bir ödünç parametreye veriliyor) literal deyim bitince bırakılır.
// Aksi halde (saklanıyor, döndürülüyor, free ediliyor, dolaylı bir çağrıya
// veriliyor) bırakılmaz: o zaman liste başka bir yerde yaşamaya devam ediyor
// olabilir.

#include "Sema.h"

#include <set>

// Ödünç alan parametreler: kaçmayanlar, ve verildikleri her parametre de ödünç
// alanlardan. İç içe / karşılıklı çağrılar için en büyük sabit nokta: başta
// kaçmayan hepsi ödünç sayılır, koşulu bozanlar çıkarılır.
void Sema::markTemporaryListArguments() {
    std::set<std::pair<std::string, int>> borrowed;
    for (auto& [function, uses] : listParamUses)
        for (auto& [index, use] : uses)
            if (!use.escapes) borrowed.insert({function, index});

    // Hiç kullanılmayan parametre de ödünçtür; ama yalnızca gövdesini
    // gördüğümüz (extern olmayan) fonksiyonlar için.
    auto isBorrowed = [&](const std::string& function, int index) {
        auto decl = functionDecls.find(function);
        if (decl == functionDecls.end() || decl->second->isExtern) return false;
        auto uses = listParamUses.find(function);
        if (uses == listParamUses.end() || !uses->second.count(index)) return true;
        return borrowed.count({function, index}) > 0;
    };

    for (bool changed = true; changed;) {
        changed = false;
        for (auto it = borrowed.begin(); it != borrowed.end();) {
            const ListParamUse& use = listParamUses[it->first][it->second];
            bool ok = true;
            for (auto& [callee, index] : use.passes) ok = ok && isBorrowed(callee, index);
            if (ok) {
                ++it;
                continue;
            }
            it = borrowed.erase(it);
            changed = true;
        }
    }

    for (auto& argument : literalArguments)
        if (argument.literal->type.isList() && isBorrowed(argument.callee, argument.index))
            argument.literal->freeAfterCall = true;
}
