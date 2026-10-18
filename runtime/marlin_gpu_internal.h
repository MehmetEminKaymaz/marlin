// marlin GPU katmanının arka uçları arasındaki iç arayüz (marlin_gpu.c ↔
// marlin_gpu_vulkan.c). Programlar bunu görmez; onlar mgpu* arayüzünü
// çağırır.

#ifndef MARLIN_GPU_INTERNAL_H
#define MARLIN_GPU_INTERNAL_H

#include <stddef.h>
#include <stdint.h>

// Programa gömülü paketin parçaları (gpu_package.h). Gösterdikleri bellek
// programın içindedir, kopyalanmaz.
typedef struct {
    const char* ptx;          // NULL: yok
    const unsigned char* spirv;
    size_t spirv_size;        // bayt; 0: yok
    const char* table;
} MarlinGpuPackage;

// --- Vulkan (marlin_gpu_vulkan.c) ---

// Vulkan sürücüsü ve hesaplama yapabilen bir aygıt var mı (bir kez denenir).
int vk_backend_available(void);
// Aygıtın karşıladığı gereksinim bitleri (MARLIN_GPU_NEEDS_*).
uint32_t vk_backend_supported(void);
// Hangi aygıtın kullanıldığı (MARLIN_GPU_REPORT için).
const char* vk_backend_device_name(void);

// `key`: paketin programdaki adresi; aynı paket bir kez yüklenir.
void* vk_module_load(const void* key, const MarlinGpuPackage* package);
void* vk_module_function(void* module, const char* name);
void vk_launch(void* function, intptr_t gridX, intptr_t gridY, intptr_t gridZ, intptr_t blockX, intptr_t blockY,
               intptr_t blockZ, void** params, size_t paramsCount);
void* vk_alloc(uint64_t size);
void vk_free(void* ptr);
void vk_memcpy(void* dst, const void* src, size_t size);
void vk_memset32(void* dst, uint32_t value, size_t count);
void vk_memset16(void* dst, uint16_t value, size_t count);
// Gönderilmiş bütün işlerin bitmesini bekler.
void vk_synchronize(void);

#endif // MARLIN_GPU_INTERNAL_H
