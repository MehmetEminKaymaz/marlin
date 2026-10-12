#ifndef MINIK_PARALLELMATMUL_H
#define MINIK_PARALLELMATMUL_H

#include <memory>

namespace mlir {
class Pass;
}

namespace minik {

// ---------------------------------------------------------------------------
// CPU'da matris çarpımını çekirdeklere dağıtma
//
// linalg.matmul sıradan indirmede iç içe üç sıralı döngü olur ve tek çekirdekte
// çalışır. Bu pass, CPU fonksiyonlarındaki her linalg.matmul'u şuna açar:
//
//   scf.parallel (%i, %j) = (0, 0) to (satir, sutun) {     sonucun her elemanı
//     scf.for %k = 0 to ortak {                             bağımsız hesaplanır
//       c[i, j] += a[i, k] * b[k, j]
//     }
//   }
//
// Sonucun elemanları birbirinden bağımsız olduğu için dış iki döngü paraleldir;
// indirme hattının sonraki adımı (async-parallel-for) onu `parallel for` ile
// aynı yoldan iş parçacıklarına dağıtır. Toplamın yapıldığı iç döngü sıralı
// kalır.
//
// `minik.gpu` işaretli fonksiyonlara dokunulmaz: oradaki matris işlemlerini
// GpuOffload GPU'ya çevirir.
// ---------------------------------------------------------------------------
std::unique_ptr<mlir::Pass> createParallelMatmulPass();

} // namespace minik

#endif // MINIK_PARALLELMATMUL_H
