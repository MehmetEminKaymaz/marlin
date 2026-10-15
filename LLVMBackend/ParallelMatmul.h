#ifndef MARLIN_PARALLELMATMUL_H
#define MARLIN_PARALLELMATMUL_H

#include <memory>

namespace mlir {
class Pass;
}

namespace marlin {

// ---------------------------------------------------------------------------
// CPU'da matris çarpımını çekirdeklere dağıtma
//
// linalg.matmul sıradan indirmede iç içe üç sıralı döngü olur, tek çekirdekte
// ve önbelleğe kötü sırayla (en içte k, B sütun boyunca) çalışır. Bu pass, CPU
// fonksiyonlarındaki her linalg.matmul'u döşemeli bir çekirdeğe açar:
//
//   * Sonuç 64 x 256'lık bloklara bölünür; bloklar birbirinden bağımsızdır ve
//     scf.parallel ile hesaplanır. İndirme hattının sonraki adımı
//     (async-parallel-for) onu `parallel for` ile aynı yoldan iş
//     parçacıklarına dağıtır.
//   * Blok içinde derinlik 256'lık şeritlerle işlenir: A ve B'nin kullanılan
//     parçaları çekirdeğin önbelleğinde kalır.
//   * En içte sonucun 6 satır x 2 vektörlük bir parçası vektör yazmaçlarında
//     biriktirilir: her adımda B'den iki vektör, A'dan altı eleman okunur.
//   * Matrisin bu sayıların katı olmayan kenarları sıradan döngülerle
//     hesaplanır.
//
// Her sonuç elemanına çarpımlar yine k sırasıyla eklenir; sonuç, döngüler
// elle yazıldığında çıkan sonuçla birebir aynıdır. Düz olmayan yerleşimli
// matrislerde (döşemeli çekirdeğin varsaydığı satır satır bitişik düzen yoksa)
// basit paralel döngüye açılır: scf.parallel (i, j) { for k: c += a * b }.
//
// `marlin.gpu` işaretli fonksiyonlara dokunulmaz: oradaki matris işlemlerini
// GpuOffload GPU'ya çevirir.
// ---------------------------------------------------------------------------
std::unique_ptr<mlir::Pass> createParallelMatmulPass();

} // namespace marlin

#endif // MARLIN_PARALLELMATMUL_H
