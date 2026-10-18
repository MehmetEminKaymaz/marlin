#ifndef MARLIN_GPUSPIRV_H
#define MARLIN_GPUSPIRV_H

#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "mlir/IR/BuiltinOps.h"

namespace marlin {

// ---------------------------------------------------------------------------
// GPU çekirdeklerinin Vulkan (SPIR-V) sürümü ve programa gömülen paket
//
// Her gpu.module (gpu-kernel-outlining'in ayırdığı çekirdek) iki arka uç için
// derlenir: NVIDIA'da CUDA (PTX; LLVMLowering.cpp, G4-G6) ve diğer GPU'larda
// Vulkan (SPIR-V; burada). İkisi, çekirdeklerin parametre tablosuyla birlikte
// tek bir pakete konup programa gömülür (biçim: runtime/gpu_package.h).
// Çalışma zamanı makinedeki GPU'ya göre birini seçer (runtime/marlin_gpu.c).
//
// SPIR-V'ye çeviri MLIR'in hazır dönüşümleriyle yapılır (gpu/scf/arith/math/
// memref → spirv). Ama onlar çekirdeği olduğu gibi kabul etmez; önce çekirdek
// Vulkan'ın modeline uydurulur:
//
//   * Vulkan çekirdeği parametre almaz, tampon bağlamaları (binding) okur.
//     Bütün sayı parametreleri tek bir i64 tamponunda (bağlama 0) toplanır;
//     her memref kendi tamponu olur (bağlama 1, 2 ...).
//   * SPIR-V dönüşümü boyutları ve adımları çalışırken belli olan memref'leri
//     (memref<?x?xf64>: matrisler) desteklemez. Her memref parametresi tek
//     boyutlu bir tampona düzleştirilir; ofset, boyutlar ve adımlar sayı
//     yuvalarından okunur, a[i][j] → tampon[ofset + i*adım0 + j*adım1] olur.
//   * Paylaşımlı bellek (workgroup attribution) ve iş parçacığının yerel
//     değişkenleri (alloca) SPIR-V'nin bellek sınıflarına yerleştirilir.
//
// Çevrilemeyen çekirdek (desteklenmeyen işlem, yapısız akış ...) derlemeyi
// durdurmaz: gereksinimine MARLIN_GPU_NO_VULKAN eklenir ve onu kullanan
// @GPU fonksiyon Vulkan'lı makinede CPU ikiziyle çalışır.
// ---------------------------------------------------------------------------

struct GpuKernelPackage {
    std::vector<uint32_t> spirv; // boşsa SPIR-V yok
    std::string table;           // çekirdek tablosu (gpu_package.h)
    uint32_t requirements = 0;   // MARLIN_GPU_* bitleri
    std::string error;           // SPIR-V üretilemediyse sebebi
};
using GpuPackages = std::map<std::string, GpuKernelPackage>; // gpu.module adı → paket

/// Modüldeki her gpu.module için SPIR-V üretir; modülün kendisi değişmez.
/// gpu-kernel-outlining'den sonra, gpu.module'ler NVVM'e çevrilmeden önce
/// çağrılmalıdır. `noCuda`: PTX üretilmeyecek (gereksinime NO_CUDA eklenir).
GpuPackages buildGpuPackages(mlir::ModuleOp module, bool noCuda);

/// @GPU fonksiyon çağrılarındaki kararı (marlin_gpu_usable) tamamlar: her
/// çağrının GPU sürümünün başlattığı çekirdeklerin (çağırdığı fonksiyonlar
/// dahil) gereksinimleri birleştirilip argüman olarak yazılır. Çağrılar kod
/// üretiminde `marlin.gpu_entry` işaretiyle üretilir. `always`: her çağrıya
/// eklenen bitler (CUDA'sız derlemede NO_CUDA: çekirdeksiz fonksiyon da
/// diğerleriyle aynı arka ucu seçsin).
void resolveGpuRequirements(mlir::ModuleOp module, const GpuPackages& packages, uint32_t always);

/// gpu.module'lerin yerine paketleri koyar:
///   * gpu-module-to-binary'den sonra (CUDA var): gpu.binary'deki PTX alınır,
///     paketle birlikte yeniden yazılır;
///   * CUDA yoksa gpu.module'ler doğrudan yalnızca SPIR-V içeren gpu.binary'ye
///     dönüştürülür.
/// Gömülen nesne her durumda "bin" biçimindedir: çalışma zamanı
/// mgpuModuleLoad(veri, boyut) ile paketin tamamını alır.
void packGpuBinaries(mlir::ModuleOp module, const GpuPackages& packages);

} // namespace marlin

#endif // MARLIN_GPUSPIRV_H
