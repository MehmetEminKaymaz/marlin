// marlin GPU paketi — derleyicinin programa gömdüğü çekirdek biçimi.
//
// Derleyici (LLVMBackend/GpuSpirv.cpp) her gpu.module için bir paket yazar;
// çalışma zamanı (marlin_gpu.c) onu mgpuModuleLoad'da okur. Bir pakette aynı
// çekirdeklerin birden fazla arka uç için derlenmiş hali bulunur:
//
//   bayt 0   "MRLNGPU1"                       sihirli sözcük (8 bayt)
//   bayt 8   u32 ptx_size                     PTX metni (NUL dahil); 0 = yok
//   bayt 12  u32 spirv_size                   SPIR-V (bayt; 4'ün katı); 0 = yok
//   bayt 16  u32 table_size                   çekirdek tablosu (NUL dahil)
//   bayt 20  u32 (ayrılmış, 0)
//   bayt 24  PTX, ardından SPIR-V, ardından tablo; her biri 4 bayta hizalı
//
// Sayılar küçük sonludur (bütün hedefler).
//
// ÇEKİRDEK TABLOSU (Vulkan için)
//
// Her çekirdek bir satırdır:
//
//   <ad> <argüman sayısı> <argüman>...
//
// Argümanlar çekirdeğin MLIR'deki parametre sırasıyla yazılır:
//   s<bayt>        sayı (index, iN, fN): CPU tarafı değerin adresini verir
//   m<rank>x<bayt> memref: CPU tarafı tanımlayıcının alanlarını ayrı ayrı
//                  verir (ayrılan adres, hizalı adres, ofset, boyutlar,
//                  adımlar — gpu-to-llvm'in çağırma biçimi)
//
// Vulkan çekirdeğinin bağlamaları (descriptor set 0):
//   0      sayı yuvaları (i64 dizisi). Sırayla: her sayı için bir yuva
//          (değerin baytları, sıfırla genişletilmiş); her memref için
//          ofset, boyutlar, adımlar (eleman cinsinden).
//   1...   her memref için bir depolama tamponu (bütün tampon; ofset yuvada)

#ifndef MARLIN_GPU_PACKAGE_H
#define MARLIN_GPU_PACKAGE_H

#define MARLIN_GPU_PACKAGE_MAGIC "MRLNGPU1"
#define MARLIN_GPU_PACKAGE_HEADER 24

// Bir çekirdeğin (ve onu çağıran @GPU fonksiyonun) çalışmak için istediği
// özellikler; derleyici çağrı noktasına yazar: marlin_gpu_usable(gereksinim).
#define MARLIN_GPU_NEEDS_FLOAT64 1u   // Vulkan: shaderFloat64
#define MARLIN_GPU_NEEDS_INT64 2u     // Vulkan: shaderInt64
#define MARLIN_GPU_NEEDS_WIDE_GROUP 8u // blokta 128'den fazla (en çok 256) iş parçacığı;
                                       // Vulkan'ın garanti ettiği 128'dir
#define MARLIN_GPU_NO_VULKAN 16u      // SPIR-V üretilemedi
#define MARLIN_GPU_NO_CUDA 32u        // PTX üretilemedi (CUDA araç takımı yok)

#endif // MARLIN_GPU_PACKAGE_H
