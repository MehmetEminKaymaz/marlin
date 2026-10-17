#ifndef MARLIN_NESTEDPARALLEL_H
#define MARLIN_NESTEDPARALLEL_H

#include <memory>

namespace mlir {
class Pass;
}

namespace marlin {

// ---------------------------------------------------------------------------
// İç içe paralel döngüler
//
// async-parallel-for her scf.parallel'i parçalara bölüp iş parçacığı havuzuna
// verir ve parçaların bitmesini BEKLER (async.await_all). Bekleyen bir havuz
// işçisi o sırada başka iş yapamaz. Bir paralel döngünün gövdesi (bir havuz
// işçisinde çalışırken) yeniden paralel döngüye girerse, her işçi kendi iç
// döngüsünün parçalarını bekler; parçaları çalıştıracak boş işçi kalmaz ve
// program kilitlenir. Kilitlenmenin olup olmaması çekirdek sayısına bağlıdır
// (dış döngünün parça sayısı işçi sayısını aşınca); bu yüzden çok çekirdekli
// makinede görünmeyip az çekirdeklide ortaya çıkar.
//
// İç içelik kaynak koddan her zaman görünmez: paralel döngünün gövdesi, kendi
// içinde paralel döngü olan bir fonksiyonu çağırabilir. Bu pass kararı
// çalışırken verir:
//
//   * Gövdesi başka bir paralel döngüye ulaşabilen (paralel döngü ya da
//     fonksiyon çağrısı içeren) her döngünün gövdesi, iş parçacığına özel bir
//     sayaçla işaretlenir: marlin_parallel_enter / marlin_parallel_exit.
//   * Her paralel döngü başta sayaca bakar (marlin_parallel_nested): bir
//     paralel döngünün içindeyse sıralı scf.for olarak çalışır, değilse
//     async-parallel-for ile dağıtılır.
//
// Böylece en dıştaki döngü bütün çekirdekleri kullanır, içtekiler işçiyi
// bekletmez. async-parallel-for'dan hemen önce çalışmalıdır.
//
// Dokunulmayanlar: sonuç döndüren (indirgemeli) döngüler ve `marlin.gpu`
// işaretli fonksiyonlar (onların döngüleri GPU'ya ayrılmıştır).
// ---------------------------------------------------------------------------
std::unique_ptr<mlir::Pass> createNestedParallelPass();

} // namespace marlin

#endif // MARLIN_NESTEDPARALLEL_H
