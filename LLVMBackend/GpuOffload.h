#ifndef MARLIN_GPUOFFLOAD_H
#define MARLIN_GPUOFFLOAD_H

#include <memory>

namespace mlir {
class Pass;
}

namespace marlin {

// ---------------------------------------------------------------------------
// GPU'ya aktarma
//
// Kod üretimi @GPU fonksiyonları da CPU fonksiyonlarıyla AYNI biçimde üretir:
// `parallel for` bir scf.parallel, matris işlemleri linalg işlemleridir.
// Fonksiyonun GPU için olduğu yalnızca `marlin.gpu` işaretinden anlaşılır.
// Bu pass işaretli fonksiyonlarda şunları yapar:
//
//   1. Matris çarpımını (linalg.matmul) kendi döşemeli çekirdeğine çevirir:
//      her blok sonucun 64x64'lük bir parçasını hesaplar; girdiler 16
//      genişliğinde şeritler halinde paylaşımlı belleğe alınır, her iş
//      parçacığı 4x4 sonucu yazmaçlarda biriktirir. (Düz çeviride her sonuç
//      elemanı bir iş parçacığı olur ve toplam her adımda GPU belleğine
//      yazılırdı; aynı veriyi her iş parçacığı yeniden okurdu.)
//      Kalan linalg işlemlerini (eleman eleman işlem, fill ...) paralel
//      döngülere açar. (İşaretsiz fonksiyonlarda bunlar sıralı döngü olur.)
//   2. Her paralel döngüyü bir GPU çekirdeği başlatan gpu.launch'a çevirir:
//
//        scf.parallel (%i, %j) = (...) to (...) step (...) { gövde }
//
//      →  %toplam = yineleme sayısı
//         gpu.launch blocks(ceil(%toplam / 256)) threads(256) {
//           %sira = blok * 256 + is_parcacigi         bu iş parçacığının sırası
//           if %sira < %toplam {
//             %i, %j = %sira'dan hesaplanır
//             gövde
//           }
//         }
//
//      GPU'da iş parçacıkları 256'lık bloklar halinde başlatılır; son bloktaki
//      fazlalık iş parçacıkları `if` sayesinde bir şey yapmaz.
//   3. Gövdeyi GPU'da çalışabilir hale getirir:
//        * gövdenin yığıt tahsisleri çekirdeğin girişine taşınır;
//        * döngü dışındaki CPU değişkenlerinin (yığıttaki yuvaların) okunması
//          döngüden önceye alınır: GPU, CPU'nun yığıtını göremez, değer
//          çekirdeğe parametre olarak geçer. (Sema, paralel döngünün bu
//          değişkenlere yazmadığını garanti eder.)
//
// Ardından MLIR'in kendi pass'leri devreye girer: gpu-kernel-outlining her
// gpu.launch gövdesini ayrı bir GPU fonksiyonuna taşır, sonrakiler onu NVIDIA
// makine koduna derler (bkz. LLVMLowering.cpp).
// ---------------------------------------------------------------------------
std::unique_ptr<mlir::Pass> createGpuOffloadPass();

// ---------------------------------------------------------------------------
// Çekirdekleri GPU derleyicisine hazırlama (gpu-kernel-outlining'den hemen önce)
//
//   * Çekirdeğin dışında üretilip içinde kullanılan sabitler çekirdeğe
//     kopyalanır. Aradaki pass'ler sabitleri fonksiyonun başına toplar;
//     dışarıda kalan sabit çekirdeğe parametre olarak geçer ve GPU derleyicisi
//     değerini bilemez (ör. adımı 1 olan döngüyü açamaz).
//   * Kayan noktalı çarpma ve toplamaların birleştirilmesine (a * b + c →
//     tek FMA komutu) izin verilir; CUDA derleyicisinin varsayılanı da budur.
// ---------------------------------------------------------------------------
std::unique_ptr<mlir::Pass> createGpuKernelPreparePass();

} // namespace marlin

#endif // MARLIN_GPUOFFLOAD_H
