#ifndef MINIK_GPUOFFLOAD_H
#define MINIK_GPUOFFLOAD_H

#include <memory>

namespace mlir {
class Pass;
}

namespace minik {

// ---------------------------------------------------------------------------
// GPU'ya aktarma
//
// Kod üretimi @GPU fonksiyonları da CPU fonksiyonlarıyla AYNI biçimde üretir:
// `parallel for` bir scf.parallel, matris işlemleri linalg işlemleridir.
// Fonksiyonun GPU için olduğu yalnızca `minik.gpu` işaretinden anlaşılır.
// Bu pass işaretli fonksiyonlarda şunları yapar:
//
//   1. linalg işlemlerini (eleman eleman işlem, matmul, fill ...) paralel
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

} // namespace minik

#endif // MINIK_GPUOFFLOAD_H
