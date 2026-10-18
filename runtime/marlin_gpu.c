// marlin GPU katmanı — @GPU fonksiyonların çalışma zamanı.
//
// Derleyici GPU işlerini MLIR'in "mgpu" arayüzüne çağrı olarak üretir
// (gpu-to-llvm: mgpuMemAlloc, mgpuMemcpy, mgpuModuleLoad, mgpuLaunchKernel
// ...). O arayüzü burada kendimiz sağlarız. MLIR'in hazır sarmalayıcısı
// (libmlir_cuda_runtime) CUDA sürücüsüne doğrudan bağlıdır: onunla derlenen
// program NVIDIA sürücüsü olmayan makinede açılmaz bile. Burada sürücüler
// çalışırken yüklenir (dlopen / LoadLibrary); bulunamazsa GPU "yok" sayılır.
//
// ARKA UÇLAR
//
//   CUDA     NVIDIA kartlar (aşağıda). Çekirdek PTX olarak gömülüdür.
//   Vulkan   diğer GPU'lar ve CUDA'sız NVIDIA kurulumları
//            (marlin_gpu_vulkan.c). Çekirdek SPIR-V olarak gömülüdür.
//
// @GPU fonksiyon her çağrılışında karar verilir (marlin_gpu_usable):
// derleyici çağrıya fonksiyonun çekirdeklerinin gereksinimlerini yazar (f64,
// i64, blok boyutu; hangi arka uç için derlenebildikleri — gpu_package.h).
// Sırayla CUDA, sonra Vulkan denenir; ikisi de karşılayamıyorsa derleyicinin
// ürettiği CPU ikizi çağrılır (bkz. MLIRBackend/MLIRGenerator.cpp, "CPU
// ikizi"). Ör. çift duyarlıklı (f64) hesap yapamayan bir GPU'da f64
// kullanan fonksiyon CPU'da çalışır. Sürücülerin kendisi programda bir kez
// yoklanır.
//
// Seçilen arka uç iş parçacığına yazılır; ardından gelen mgpu* çağrıları
// (aynı iş parçacığında, GPU sürümünün içinden) ona gider. İç içe @GPU
// çağrısı da aynı arka ucu seçer: dıştaki çağrının gereksinimleri içtekileri
// kapsar.
//
//   MARLIN_GPU=cpu      GPU olsa da CPU ikizleri kullanılır (testler iki
//                       yolu da böyle sınar)
//   MARLIN_GPU=cuda     yalnızca CUDA (yoksa CPU)
//   MARLIN_GPU=vulkan   yalnızca Vulkan (yoksa CPU)
//   MARLIN_GPU_REPORT=1 kullanılan arka ucu (ilk seçildiğinde) stderr'e yazar
//
// CUDA ile konuşma biçimi MLIR'in CudaRuntimeWrappers.cpp'siyle aynıdır
// (Apache-2.0 WITH LLVM-exception; LLVM Project): birincil bağlam, her çağrıda
// iş parçacığına bağlanıp bırakılır; böylece görevler (marl) farklı iş
// parçacıklarından GPU kullanabilir.

#include "gpu_package.h"
#include "marlin_gpu_internal.h"
#include "marlin_rt.h"
#include "platform.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// ---------------------------------------------------------------------------
// CUDA sürücü API'sinin kullanılan parçası (cuda.h olmadan)
// ---------------------------------------------------------------------------

typedef int CUresult;
typedef int CUdevice;
typedef void* CUcontext;
typedef void* CUmodule;
typedef void* CUfunction;
typedef void* CUstream;
typedef void* CUevent;
typedef unsigned long long CUdeviceptr;

#define CUDA_SUCCESS 0
#define CU_STREAM_NON_BLOCKING 1
#define CU_EVENT_DEFAULT 0

// Sürücüdeki adları: bazıları _v2 ile dışa açılır (cuda.h makrolarla eşler).
#define CUDA_FUNCTIONS(X)                                                              \
    X(cuInit, "cuInit", CUresult, (unsigned))                                          \
    X(cuDeviceGetCount, "cuDeviceGetCount", CUresult, (int*))                          \
    X(cuDeviceGet, "cuDeviceGet", CUresult, (CUdevice*, int))                          \
    X(cuDeviceGetName, "cuDeviceGetName", CUresult, (char*, int, CUdevice))            \
    X(cuDevicePrimaryCtxRetain, "cuDevicePrimaryCtxRetain", CUresult, (CUcontext*, CUdevice)) \
    X(cuCtxPushCurrent, "cuCtxPushCurrent_v2", CUresult, (CUcontext))                  \
    X(cuCtxPopCurrent, "cuCtxPopCurrent_v2", CUresult, (CUcontext*))                   \
    X(cuGetErrorString, "cuGetErrorString", CUresult, (CUresult, const char**))        \
    X(cuModuleLoadData, "cuModuleLoadData", CUresult, (CUmodule*, const void*))        \
    X(cuModuleUnload, "cuModuleUnload", CUresult, (CUmodule))                          \
    X(cuModuleGetFunction, "cuModuleGetFunction", CUresult, (CUfunction*, CUmodule, const char*)) \
    X(cuLaunchKernel, "cuLaunchKernel", CUresult,                                      \
      (CUfunction, unsigned, unsigned, unsigned, unsigned, unsigned, unsigned, unsigned, \
       CUstream, void**, void**))                                                      \
    X(cuStreamCreate, "cuStreamCreate", CUresult, (CUstream*, unsigned))               \
    X(cuStreamDestroy, "cuStreamDestroy_v2", CUresult, (CUstream))                     \
    X(cuStreamSynchronize, "cuStreamSynchronize", CUresult, (CUstream))                \
    X(cuStreamWaitEvent, "cuStreamWaitEvent", CUresult, (CUstream, CUevent, unsigned)) \
    X(cuEventCreate, "cuEventCreate", CUresult, (CUevent*, unsigned))                  \
    X(cuEventDestroy, "cuEventDestroy_v2", CUresult, (CUevent))                        \
    X(cuEventSynchronize, "cuEventSynchronize", CUresult, (CUevent))                   \
    X(cuEventRecord, "cuEventRecord", CUresult, (CUevent, CUstream))                   \
    X(cuMemAlloc, "cuMemAlloc_v2", CUresult, (CUdeviceptr*, size_t))                   \
    X(cuMemAllocManaged, "cuMemAllocManaged", CUresult, (CUdeviceptr*, size_t, unsigned)) \
    X(cuMemFree, "cuMemFree_v2", CUresult, (CUdeviceptr))                              \
    X(cuMemcpyAsync, "cuMemcpyAsync", CUresult, (CUdeviceptr, CUdeviceptr, size_t, CUstream)) \
    X(cuMemsetD32Async, "cuMemsetD32Async", CUresult, (CUdeviceptr, unsigned, size_t, CUstream)) \
    X(cuMemsetD16Async, "cuMemsetD16Async", CUresult, (CUdeviceptr, unsigned short, size_t, CUstream)) \
    X(cuMemHostRegister, "cuMemHostRegister_v2", CUresult, (void*, size_t, unsigned))  \
    X(cuMemHostUnregister, "cuMemHostUnregister", CUresult, (void*))

// Akış sıralı bellek havuzu (CUDA 11.2+; eski sürücüde yoktur, o zaman
// cuMemAlloc / cuMemFree kullanılır).
typedef void* CUmemoryPool;
#define CU_MEMPOOL_ATTR_RELEASE_THRESHOLD 4
#define CUDA_POOL_FUNCTIONS(X)                                                         \
    X(cuMemAllocAsync, "cuMemAllocAsync", CUresult, (CUdeviceptr*, size_t, CUstream))  \
    X(cuMemFreeAsync, "cuMemFreeAsync", CUresult, (CUdeviceptr, CUstream))             \
    X(cuDeviceGetDefaultMemPool, "cuDeviceGetDefaultMemPool", CUresult, (CUmemoryPool*, CUdevice)) \
    X(cuMemPoolSetAttribute, "cuMemPoolSetAttribute", CUresult, (CUmemoryPool, int, void*))

#define DECLARE_POINTER(field, symbol, ret, params) ret(*field) params;
static struct {
    CUDA_FUNCTIONS(DECLARE_POINTER)
    CUDA_POOL_FUNCTIONS(DECLARE_POINTER)
} cuda;
#undef DECLARE_POINTER
static int cudaPool; // havuz kullanılabiliyor

static CUcontext context; // birincil bağlam (cihaz 0)
static char cudaDeviceName[256];

// ---------------------------------------------------------------------------
// Sürücüler: hangisi var? (her biri bir kez yoklanır)
// ---------------------------------------------------------------------------

// Sürücü kütüphanesinin platforma göre adı.
static void* open_cuda_driver(void) {
#if defined(_WIN32)
    return mr_library_open("nvcuda.dll");
#elif defined(__APPLE__)
    return NULL; // macOS'ta CUDA yok
#else
    void* library = mr_library_open("libcuda.so.1");
    return library ? library : mr_library_open("libcuda.so");
#endif
}

static mr_mutex cudaModulesLock;

static int detect_cuda(void) {
    mr_mutex_init(&cudaModulesLock);
    void* library = open_cuda_driver();
    if (!library) return 0;
#define LOAD_POINTER(field, symbol, ret, params)                          \
    cuda.field = (ret(*) params)mr_library_symbol(library, symbol);      \
    if (!cuda.field) return 0;
    CUDA_FUNCTIONS(LOAD_POINTER)
#undef LOAD_POINTER

    int count = 0;
    CUdevice device;
    if (cuda.cuInit(0) != CUDA_SUCCESS) return 0;
    if (cuda.cuDeviceGetCount(&count) != CUDA_SUCCESS || count == 0) return 0;
    if (cuda.cuDeviceGet(&device, 0) != CUDA_SUCCESS) return 0;
    if (cuda.cuDevicePrimaryCtxRetain(&context, device) != CUDA_SUCCESS) return 0;
    cuda.cuDeviceGetName(cudaDeviceName, (int)sizeof cudaDeviceName, device);

    // Bellek havuzu: GPU belleği ayırmak ve bırakmak (cuMemAlloc / cuMemFree)
    // her biri ~0.1 ms sürer ve bırakma bütün aygıtı bekletir; @GPU çağrısı
    // başına birkaç kez yapılır. Havuzda bırakılan blok sürücüye geri
    // verilmez, sonraki ayırmada yeniden kullanılır. Varsayılan havuz boşta
    // kalan belleği her beklemede sürücüye iade eder; eşik en yükseğe
    // çekilerek program boyunca tutulur. (Bellek sıkışırsa sürücü yine
    // havuzdan geri alır.)
#define LOAD_POOL(field, symbol, ret, params) cuda.field = (ret(*) params)mr_library_symbol(library, symbol);
    CUDA_POOL_FUNCTIONS(LOAD_POOL)
#undef LOAD_POOL
    CUmemoryPool pool;
    uint64_t keepAll = UINT64_MAX;
    cudaPool = cuda.cuMemAllocAsync && cuda.cuMemFreeAsync && cuda.cuDeviceGetDefaultMemPool &&
               cuda.cuMemPoolSetAttribute && cuda.cuDeviceGetDefaultMemPool(&pool, device) == CUDA_SUCCESS &&
               cuda.cuMemPoolSetAttribute(pool, CU_MEMPOOL_ATTR_RELEASE_THRESHOLD, &keepAll) == CUDA_SUCCESS;
    return 1;
}

// 0 bilinmiyor, 1 karar veriliyor, 2 yok, 3 var.
static int cudaState = 0;

static int cuda_available(void) {
    int s = __atomic_load_n(&cudaState, __ATOMIC_ACQUIRE);
    if (s >= 2) return s == 3;
    int expected = 0;
    if (__atomic_compare_exchange_n(&cudaState, &expected, 1, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
        __atomic_store_n(&cudaState, detect_cuda() ? 3 : 2, __ATOMIC_RELEASE);
    } else {
        while (__atomic_load_n(&cudaState, __ATOMIC_ACQUIRE) == 1) mr_sleep_seconds(0.001);
    }
    return __atomic_load_n(&cudaState, __ATOMIC_ACQUIRE) == 3;
}

// MARLIN_GPU (bir kez okunur): hangi arka uçlara izin var.
enum { ALLOW_CUDA = 1, ALLOW_VULKAN = 2 };
static int allowed = -1;

static int allowed_backends(void) {
    int a = __atomic_load_n(&allowed, __ATOMIC_ACQUIRE);
    if (a >= 0) return a;
    char* choice = mr_getenv("MARLIN_GPU");
    a = ALLOW_CUDA | ALLOW_VULKAN;
    if (choice) {
        if (strcmp(choice, "cpu") == 0 || strcmp(choice, "off") == 0) a = 0;
        else if (strcmp(choice, "cuda") == 0) a = ALLOW_CUDA;
        else if (strcmp(choice, "vulkan") == 0) a = ALLOW_VULKAN;
    }
    free(choice);
    __atomic_store_n(&allowed, a, __ATOMIC_RELEASE);
    return a;
}

// ---------------------------------------------------------------------------
// Karar
// ---------------------------------------------------------------------------

enum { BACKEND_NONE = 0, BACKEND_CUDA = 1, BACKEND_VULKAN = 2 };
static _Thread_local int backend; // bu iş parçacığındaki @GPU çağrısının arka ucu

static const uint32_t NEEDS = MARLIN_GPU_NEEDS_FLOAT64 | MARLIN_GPU_NEEDS_INT64 | MARLIN_GPU_NEEDS_WIDE_GROUP;

// Rapor (MARLIN_GPU_REPORT): her farklı karar bir kez yazılır. GPU varken
// CPU'ya düşülürse sebebi de yazılır:
//   marlin gpu: cuda (aygıt)
//   marlin gpu: vulkan (aygıt)
//   marlin gpu: cpu (vulkan: float64 yok)        aygıt fonksiyonun istediğini yapamıyor
//   marlin gpu: cpu (vulkan: cekirdek derlenemedi) çekirdeğin SPIR-V'si yok
//   marlin gpu: cpu                               kullanılabilir GPU yok
// Anahtar: 0-7 cpu (eksik özellik maskesi; 0 = GPU yok), 8 SPIR-V yok,
// 9 cuda, 10 vulkan.
static int reported[11];

static void report_choice(int key, uint32_t missing) {
    if (__atomic_exchange_n(&reported[key], 1, __ATOMIC_ACQ_REL)) return;
    char* wanted = mr_getenv("MARLIN_GPU_REPORT");
    const int show = wanted && strcmp(wanted, "0") != 0;
    free(wanted);
    if (!show) return;
    if (key == 9) fprintf(stderr, "marlin gpu: cuda (%s)\n", cudaDeviceName);
    else if (key == 10) fprintf(stderr, "marlin gpu: vulkan (%s)\n", vk_backend_device_name());
    else if (key == 8) fprintf(stderr, "marlin gpu: cpu (vulkan: cekirdek derlenemedi)\n");
    else if (missing == 0) fprintf(stderr, "marlin gpu: cpu\n");
    else
        fprintf(stderr, "marlin gpu: cpu (vulkan:%s%s%s yok)\n", (missing & MARLIN_GPU_NEEDS_FLOAT64) ? " float64" : "",
                (missing & MARLIN_GPU_NEEDS_INT64) ? " int64" : "",
                (missing & MARLIN_GPU_NEEDS_WIDE_GROUP) ? " 256'lik blok" : "");
}

int32_t marlin_gpu_usable(int32_t requirements) {
    const uint32_t r = (uint32_t)requirements;
    const int a = allowed_backends();
    int chosen = BACKEND_NONE, key = 0;
    uint32_t missing = 0;
    if ((a & ALLOW_CUDA) && !(r & MARLIN_GPU_NO_CUDA) && cuda_available()) {
        chosen = BACKEND_CUDA; // NVIDIA: f64, i64 ve 256'lık bloklar her kartta var
        key = 9;
    } else if ((a & ALLOW_VULKAN) && vk_backend_available()) {
        missing = r & NEEDS & ~vk_backend_supported();
        if (r & MARLIN_GPU_NO_VULKAN) {
            key = 8;
        } else if (missing) {
            // (bitler 1, 2, 8 → anahtar 1-7)
            key = ((missing & MARLIN_GPU_NEEDS_FLOAT64) ? 1 : 0) | ((missing & MARLIN_GPU_NEEDS_INT64) ? 2 : 0) |
                  ((missing & MARLIN_GPU_NEEDS_WIDE_GROUP) ? 4 : 0);
        } else {
            chosen = BACKEND_VULKAN;
            key = 10;
        }
    }
    backend = chosen;
    report_choice(key, missing);
    return chosen != BACKEND_NONE;
}

// ---------------------------------------------------------------------------
// Yardımcılar (CUDA)
// ---------------------------------------------------------------------------

// MLIR'in sarmalayıcısıyla aynı biçim: "'cuX(...)' failed with 'açıklama'".
// (Hata programı durdurmaz; tests/run.sh bu satırı arar.)
static void report(CUresult result, const char* call) {
    if (result == CUDA_SUCCESS) return;
    const char* text = "<bilinmeyen>";
    if (cuda.cuGetErrorString) cuda.cuGetErrorString(result, &text);
    fprintf(stderr, "'%s' failed with '%s'\n", call, text);
}
#define CHECK(call) report(cuda.call, #call)

// Birincil bağlam çağrı boyunca bu iş parçacığına bağlanır.
static void enter(void) { CHECK(cuCtxPushCurrent(context)); }

static void leave(void) {
    CUcontext previous;
    CHECK(cuCtxPopCurrent(&previous));
}

static int on_vulkan(void) {
    if (backend == BACKEND_VULKAN) return 1;
    if (backend != BACKEND_CUDA) marlin_rt_panic("GPU arka ucu secilmeden GPU islemi");
    return 0;
}

// Paketin parçaları (biçim: gpu_package.h).
static uint32_t read_u32(const unsigned char* p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static MarlinGpuPackage open_package(const void* data, size_t size) {
    const unsigned char* bytes = data;
    if (size < MARLIN_GPU_PACKAGE_HEADER || memcmp(bytes, MARLIN_GPU_PACKAGE_MAGIC, 8) != 0)
        marlin_rt_panic("GPU paketi bozuk");
    const uint32_t ptxSize = read_u32(bytes + 8), spirvSize = read_u32(bytes + 12);
    const size_t ptxAt = MARLIN_GPU_PACKAGE_HEADER;
    const size_t spirvAt = ptxAt + ((ptxSize + 3u) & ~3u);
    const size_t tableAt = spirvAt + spirvSize;
    MarlinGpuPackage package;
    package.ptx = ptxSize ? (const char*)bytes + ptxAt : NULL;
    package.spirv = bytes + spirvAt;
    package.spirv_size = spirvSize;
    package.table = (const char*)bytes + tableAt;
    return package;
}

// ---------------------------------------------------------------------------
// mgpu arayüzü (imzalar MLIR'in gpu-to-llvm dönüşümünün çağırdığıyla aynı)
// ---------------------------------------------------------------------------

// Paket her çekirdek başlatmada yeniden "yüklenir" ve ardından bırakılır
// (MLIR 19'un ürettiği kod böyledir). Yükleme pahalıdır: CUDA'da sürücü
// PTX'i her seferinde yeniden işler. Paket programın içinde sabit bir
// adreste durduğu için o adrese göre bir kez yüklenir ve program boyunca
// saklanır; "bırakma" bir şey yapmaz. (Vulkan arka ucu kendi içinde aynısını
// yapar.)
typedef struct CudaModule {
    struct CudaModule* next;
    const void* key;
    CUmodule module;
} CudaModule;

static CudaModule* cudaModules; // (kilidi cudaModulesLock, detect_cuda'da kurulur)

void* mgpuModuleLoad(void* data, size_t size) {
    const MarlinGpuPackage package = open_package(data, size);
    if (on_vulkan()) return vk_module_load(data, &package);
    if (!package.ptx) marlin_rt_panic("GPU paketinde NVIDIA cekirdegi yok");
    mr_mutex_lock(&cudaModulesLock);
    for (CudaModule* m = cudaModules; m; m = m->next) {
        if (m->key == data) {
            mr_mutex_unlock(&cudaModulesLock);
            return m->module;
        }
    }
    CudaModule* entry = calloc(1, sizeof *entry);
    if (!entry) marlin_rt_panic("bellek yetersiz");
    entry->key = data;
    enter();
    CHECK(cuModuleLoadData(&entry->module, package.ptx));
    leave();
    entry->next = cudaModules;
    cudaModules = entry;
    mr_mutex_unlock(&cudaModulesLock);
    return entry->module;
}

void mgpuModuleUnload(void* module) { (void)module; } // saklanır (yukarıya bakın)

void* mgpuModuleGetFunction(void* module, const char* name) {
    if (on_vulkan()) return vk_module_function(module, name);
    CUfunction function = NULL;
    CHECK(cuModuleGetFunction(&function, module, name));
    return function;
}

void mgpuLaunchKernel(void* function, intptr_t gridX, intptr_t gridY, intptr_t gridZ, intptr_t blockX,
                      intptr_t blockY, intptr_t blockZ, int32_t sharedMemory, void* stream, void** params,
                      void** extra, size_t paramsCount) {
    if (on_vulkan()) {
        vk_launch(function, gridX, gridY, gridZ, blockX, blockY, blockZ, params, paramsCount);
        return;
    }
    enter();
    CHECK(cuLaunchKernel(function, (unsigned)gridX, (unsigned)gridY, (unsigned)gridZ, (unsigned)blockX,
                         (unsigned)blockY, (unsigned)blockZ, (unsigned)sharedMemory, stream, params, extra));
    leave();
}

// Vulkan'da akış ve olay yoktur: bütün işler tek kuyrukta sıralıdır;
// beklemek, biriken işi gönderip bitmesini beklemektir. (Programa NULL
// olmayan bir tutamaç verilir.)
static char vulkanStream;

// Üretilen kod her küçük GPU işi için bir akış açar, bekler ve siler (@GPU
// çağrısı başına birkaç kez). Silinen akış atılmaz, sonraki açılışta yeniden
// verilir: üretilen kod akışı silmeden önce hep bekler, yani boştadır.
#define CUDA_SPARE_STREAMS 64
static CUstream spareStreams[CUDA_SPARE_STREAMS];
static int spareStreamCount; // (kilidi cudaModulesLock)

void* mgpuStreamCreate(void) {
    if (on_vulkan()) return &vulkanStream;
    CUstream stream = NULL;
    mr_mutex_lock(&cudaModulesLock);
    if (spareStreamCount > 0) stream = spareStreams[--spareStreamCount];
    mr_mutex_unlock(&cudaModulesLock);
    if (stream) return stream;
    enter();
    CHECK(cuStreamCreate(&stream, CU_STREAM_NON_BLOCKING));
    leave();
    return stream;
}

void mgpuStreamDestroy(void* stream) {
    if (on_vulkan()) return;
    mr_mutex_lock(&cudaModulesLock);
    const int kept = spareStreamCount < CUDA_SPARE_STREAMS;
    if (kept) spareStreams[spareStreamCount++] = stream;
    mr_mutex_unlock(&cudaModulesLock);
    if (!kept) CHECK(cuStreamDestroy(stream));
}

void mgpuStreamSynchronize(void* stream) {
    if (on_vulkan()) {
        vk_synchronize();
        return;
    }
    CHECK(cuStreamSynchronize(stream));
}

void mgpuStreamWaitEvent(void* stream, void* event) {
    if (on_vulkan()) return;
    CHECK(cuStreamWaitEvent(stream, event, 0));
}

void* mgpuEventCreate(void) {
    if (on_vulkan()) return &vulkanStream;
    CUevent event = NULL;
    enter();
    CHECK(cuEventCreate(&event, CU_EVENT_DEFAULT));
    leave();
    return event;
}

void mgpuEventDestroy(void* event) {
    if (on_vulkan()) return;
    CHECK(cuEventDestroy(event));
}

void mgpuEventSynchronize(void* event) {
    if (on_vulkan()) {
        vk_synchronize();
        return;
    }
    CHECK(cuEventSynchronize(event));
}

void mgpuEventRecord(void* event, void* stream) {
    if (on_vulkan()) return;
    CHECK(cuEventRecord(event, stream));
}

// Ayırma ve bırakma, verilen akışın sırasındadır (havuz): blok, akışta
// önceki işler bitince bırakılmış sayılır. Üretilen kod her ayırma/bırakma
// grubundan sonra akışı bekler; farklı akışlardaki işler birbirine karışmaz.
//
// Ortak bellek (isHostShared: CPU ile paylaşılan) havuzdan gelmez; onu
// bırakmak için hangi adreslerin ortak olduğu ayrıca tutulur (nadirdir).
typedef struct SharedBlock {
    struct SharedBlock* next;
    void* ptr;
} SharedBlock;
static SharedBlock* sharedBlocks; // (kilidi cudaModulesLock)

static int take_shared(void* ptr) {
    int found = 0;
    mr_mutex_lock(&cudaModulesLock);
    for (SharedBlock** b = &sharedBlocks; *b; b = &(*b)->next) {
        if ((*b)->ptr == ptr) {
            SharedBlock* gone = *b;
            *b = gone->next;
            free(gone);
            found = 1;
            break;
        }
    }
    mr_mutex_unlock(&cudaModulesLock);
    return found;
}

void* mgpuMemAlloc(uint64_t sizeBytes, void* stream, bool isHostShared) {
    if (on_vulkan()) return vk_alloc(sizeBytes);
    CUdeviceptr ptr = 0;
    if (sizeBytes == 0) return NULL;
    enter();
    if (isHostShared) {
        CHECK(cuMemAllocManaged(&ptr, sizeBytes, /*CU_MEM_ATTACH_GLOBAL*/ 1));
        SharedBlock* block = malloc(sizeof *block);
        if (!block) marlin_rt_panic("bellek yetersiz");
        block->ptr = (void*)(uintptr_t)ptr;
        mr_mutex_lock(&cudaModulesLock);
        block->next = sharedBlocks;
        sharedBlocks = block;
        mr_mutex_unlock(&cudaModulesLock);
    } else if (cudaPool) {
        CHECK(cuMemAllocAsync(&ptr, sizeBytes, stream));
    } else {
        CHECK(cuMemAlloc(&ptr, sizeBytes));
    }
    leave();
    return (void*)(uintptr_t)ptr;
}

void mgpuMemFree(void* ptr, void* stream) {
    if (on_vulkan()) {
        vk_free(ptr);
        return;
    }
    if (!ptr) return;
    if (cudaPool && !take_shared(ptr))
        CHECK(cuMemFreeAsync((CUdeviceptr)(uintptr_t)ptr, stream));
    else
        CHECK(cuMemFree((CUdeviceptr)(uintptr_t)ptr));
}

void mgpuMemcpy(void* dst, void* src, size_t sizeBytes, void* stream) {
    if (on_vulkan()) {
        vk_memcpy(dst, src, sizeBytes);
        return;
    }
    CHECK(cuMemcpyAsync((CUdeviceptr)(uintptr_t)dst, (CUdeviceptr)(uintptr_t)src, sizeBytes, stream));
}

void mgpuMemset32(void* dst, unsigned value, size_t count, void* stream) {
    if (on_vulkan()) {
        vk_memset32(dst, value, count);
        return;
    }
    CHECK(cuMemsetD32Async((CUdeviceptr)(uintptr_t)dst, value, count, stream));
}

void mgpuMemset16(void* dst, unsigned short value, size_t count, void* stream) {
    if (on_vulkan()) {
        vk_memset16(dst, value, count);
        return;
    }
    CHECK(cuMemsetD16Async((CUdeviceptr)(uintptr_t)dst, value, count, stream));
}

// CPU belleğini GPU'ya açmak: Vulkan'da gerekmez (kullanılmaz).
void mgpuMemHostRegister(void* ptr, uint64_t sizeBytes) {
    if (on_vulkan()) return;
    enter();
    CHECK(cuMemHostRegister(ptr, sizeBytes, 0));
    leave();
}

void mgpuMemHostUnregister(void* ptr) {
    if (on_vulkan()) return;
    enter();
    CHECK(cuMemHostUnregister(ptr));
    leave();
}

void mgpuSetDefaultDevice(int32_t device) { (void)device; } // (yalnızca cihaz 0)
