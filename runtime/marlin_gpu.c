// marlin GPU katmanı — @GPU fonksiyonların çalışma zamanı.
//
// Derleyici GPU işlerini MLIR'in "mgpu" arayüzüne çağrı olarak üretir
// (gpu-to-llvm: mgpuMemAlloc, mgpuMemcpy, mgpuModuleLoad, mgpuLaunchKernel
// ...). O arayüzü burada kendimiz sağlarız. MLIR'in hazır sarmalayıcısı
// (libmlir_cuda_runtime) CUDA sürücüsüne doğrudan bağlıdır: onunla derlenen
// program NVIDIA sürücüsü olmayan makinede açılmaz bile. Burada sürücü
// çalışırken yüklenir (dlopen / LoadLibrary); bulunamazsa GPU "yok" sayılır.
//
// ARKA UÇLAR
//
// Şimdilik CUDA (NVIDIA). Program açıkken bir kez karar verilir
// (marlin_gpu_available): GPU kullanılabiliyorsa @GPU fonksiyonların GPU
// sürümü, değilse derleyicinin ürettiği CPU ikizi çağrılır (bkz.
// MLIRBackend/MLIRGenerator.cpp, "CPU ikizi"). Vulkan arka ucu aynı katmana
// eklenecektir.
//
//   MARLIN_GPU=cpu    GPU olsa da CPU ikizleri kullanılır (testler iki yolu
//                     da böyle sınar)
//
// CUDA ile konuşma biçimi MLIR'in CudaRuntimeWrappers.cpp'siyle aynıdır
// (Apache-2.0 WITH LLVM-exception; LLVM Project): birincil bağlam, her çağrıda
// iş parçacığına bağlanıp bırakılır; böylece görevler (marl) farklı iş
// parçacıklarından GPU kullanabilir.

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

#define DECLARE_POINTER(field, symbol, ret, params) ret(*field) params;
static struct {
    CUDA_FUNCTIONS(DECLARE_POINTER)
} cuda;
#undef DECLARE_POINTER

static CUcontext context; // birincil bağlam (cihaz 0)

// ---------------------------------------------------------------------------
// Karar: GPU kullanılabilir mi?
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

static int detect(void) {
    char* choice = mr_getenv("MARLIN_GPU");
    const int forcedCpu = choice && (strcmp(choice, "cpu") == 0 || strcmp(choice, "off") == 0);
    free(choice);
    if (forcedCpu) return 0;

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
    return 1;
}

// Programda bir kez karar verilir; sonuç iş parçacıkları arasında paylaşılır.
// 0 bilinmiyor, 1 karar veriliyor, 2 GPU yok, 3 GPU var.
static int state = 0;

int32_t marlin_gpu_available(void) {
    int s = __atomic_load_n(&state, __ATOMIC_ACQUIRE);
    if (s >= 2) return s == 3;
    int expected = 0;
    if (__atomic_compare_exchange_n(&state, &expected, 1, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
        __atomic_store_n(&state, detect() ? 3 : 2, __ATOMIC_RELEASE);
    } else {
        while (__atomic_load_n(&state, __ATOMIC_ACQUIRE) == 1) mr_sleep_seconds(0.001);
    }
    return __atomic_load_n(&state, __ATOMIC_ACQUIRE) == 3;
}

// ---------------------------------------------------------------------------
// Yardımcılar
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

static void require_gpu(void) {
    if (!marlin_gpu_available())
        marlin_rt_panic("GPU calisma zamani kullanilamiyor (NVIDIA surucusu bulunamadi)");
}

// Birincil bağlam çağrı boyunca bu iş parçacığına bağlanır.
static void enter(void) {
    require_gpu();
    CHECK(cuCtxPushCurrent(context));
}

static void leave(void) {
    CUcontext previous;
    CHECK(cuCtxPopCurrent(&previous));
}

// ---------------------------------------------------------------------------
// mgpu arayüzü (imzalar MLIR'in gpu-to-llvm dönüşümünün çağırdığıyla aynı)
// ---------------------------------------------------------------------------

void* mgpuModuleLoad(void* data, size_t size) {
    (void)size;
    CUmodule module = NULL;
    enter();
    CHECK(cuModuleLoadData(&module, data));
    leave();
    return module;
}

// Çekirdek PTX metni olarak gömülmüşse (derleme makinesinde ptxas yoksa):
// sürücü yüklerken makine koduna çevirir (ve kendi ön belleğinde saklar).
void* mgpuModuleLoadJIT(void* data, int optLevel) {
    (void)optLevel;
    return mgpuModuleLoad(data, 0);
}

void mgpuModuleUnload(void* module) { CHECK(cuModuleUnload(module)); }

void* mgpuModuleGetFunction(void* module, const char* name) {
    CUfunction function = NULL;
    CHECK(cuModuleGetFunction(&function, module, name));
    return function;
}

void mgpuLaunchKernel(void* function, intptr_t gridX, intptr_t gridY, intptr_t gridZ, intptr_t blockX,
                      intptr_t blockY, intptr_t blockZ, int32_t sharedMemory, void* stream, void** params,
                      void** extra, size_t paramsCount) {
    (void)paramsCount;
    enter();
    CHECK(cuLaunchKernel(function, (unsigned)gridX, (unsigned)gridY, (unsigned)gridZ, (unsigned)blockX,
                         (unsigned)blockY, (unsigned)blockZ, (unsigned)sharedMemory, stream, params, extra));
    leave();
}

void* mgpuStreamCreate(void) {
    CUstream stream = NULL;
    enter();
    CHECK(cuStreamCreate(&stream, CU_STREAM_NON_BLOCKING));
    leave();
    return stream;
}

void mgpuStreamDestroy(void* stream) { CHECK(cuStreamDestroy(stream)); }
void mgpuStreamSynchronize(void* stream) { CHECK(cuStreamSynchronize(stream)); }
void mgpuStreamWaitEvent(void* stream, void* event) { CHECK(cuStreamWaitEvent(stream, event, 0)); }

void* mgpuEventCreate(void) {
    CUevent event = NULL;
    enter();
    CHECK(cuEventCreate(&event, CU_EVENT_DEFAULT));
    leave();
    return event;
}

void mgpuEventDestroy(void* event) { CHECK(cuEventDestroy(event)); }
void mgpuEventSynchronize(void* event) { CHECK(cuEventSynchronize(event)); }
void mgpuEventRecord(void* event, void* stream) { CHECK(cuEventRecord(event, stream)); }

void* mgpuMemAlloc(uint64_t sizeBytes, void* stream, bool isHostShared) {
    (void)stream;
    CUdeviceptr ptr = 0;
    if (sizeBytes == 0) return NULL;
    enter();
    if (isHostShared)
        CHECK(cuMemAllocManaged(&ptr, sizeBytes, /*CU_MEM_ATTACH_GLOBAL*/ 1));
    else
        CHECK(cuMemAlloc(&ptr, sizeBytes));
    leave();
    return (void*)(uintptr_t)ptr;
}

void mgpuMemFree(void* ptr, void* stream) {
    (void)stream;
    if (ptr) CHECK(cuMemFree((CUdeviceptr)(uintptr_t)ptr));
}

void mgpuMemcpy(void* dst, void* src, size_t sizeBytes, void* stream) {
    CHECK(cuMemcpyAsync((CUdeviceptr)(uintptr_t)dst, (CUdeviceptr)(uintptr_t)src, sizeBytes, stream));
}

void mgpuMemset32(void* dst, unsigned value, size_t count, void* stream) {
    CHECK(cuMemsetD32Async((CUdeviceptr)(uintptr_t)dst, value, count, stream));
}

void mgpuMemset16(void* dst, unsigned short value, size_t count, void* stream) {
    CHECK(cuMemsetD16Async((CUdeviceptr)(uintptr_t)dst, value, count, stream));
}

void mgpuMemHostRegister(void* ptr, uint64_t sizeBytes) {
    enter();
    CHECK(cuMemHostRegister(ptr, sizeBytes, 0));
    leave();
}

void mgpuMemHostUnregister(void* ptr) {
    enter();
    CHECK(cuMemHostUnregister(ptr));
    leave();
}

void mgpuSetDefaultDevice(int32_t device) { (void)device; } // (yalnızca cihaz 0)
