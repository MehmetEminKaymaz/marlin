// marlin GPU katmanı — Vulkan arka ucu.
//
// NVIDIA dışındaki GPU'lar (AMD, Intel, Apple: MoltenVK) ve CUDA'sız NVIDIA
// kurulumları için. Çekirdekler derleyicinin pakete koyduğu SPIR-V'dir (bkz.
// LLVMBackend/GpuSpirv.h, runtime/gpu_package.h). Vulkan kütüphanesi
// çalışırken yüklenir (libvulkan.so.1 / vulkan-1.dll / libvulkan.1.dylib);
// bulunamazsa ya da hesaplama yapabilen aygıt yoksa arka uç "yok" sayılır.
//
// MODEL
//
//   Bellek     her mgpuMemAlloc ayrı bir VkBuffer'dır; CPU'nun da görebildiği
//              (HOST_VISIBLE | HOST_COHERENT) bellekte açılır ve kalıcı
//              olarak eşlenir. Programa eşlenen adres verilir: kopyalama
//              (mgpuMemcpy) GPU'daki işler bittikten sonra düz memcpy'dir.
//              Çekirdeğe giden adresten ait olduğu tampon bulunur. Aygıtın
//              kendi belleği CPU'ya açıksa (DEVICE_LOCAL | HOST_VISIBLE:
//              tümleşik GPU'lar, Apple, ReBAR'lı kartlar) o tercih edilir.
//   Çekirdek   her çekirdek bir hesaplama hattıdır (VkPipeline); paketle
//              birlikte bir kez oluşturulur, program boyunca saklanır.
//   Başlatma   komutlar tek bir komut tamponunda biriktirilir; ardışık
//              çekirdekler arasına bellek bariyeri konur. Tampon, CPU'nun
//              sonucu beklediği yerde (kopyalama, akış/olay bekleme, bellek
//              bırakma) gönderilir ve bitmesi beklenir.
//   Parametre  sayılar ve memref yerleşimleri (ofset, boyut, adım) her
//              başlatma için küçük bir "yuva" tamponuna (bağlama 0) yazılır;
//              memref'lerin tamponları bağlama 1, 2 ...
//
// Bütün Vulkan erişimi tek kilitle sıralanır (kuyruk ve komut tamponu
// paylaşılır); görevler (marl) farklı iş parçacıklarından GPU kullanabilir.

#include "marlin_gpu_internal.h"

#include "gpu_package.h"
#include "marlin_rt.h"
#include "platform.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef MARLIN_HAVE_VULKAN

// Çalışma zamanı Vulkan başlıkları olmadan derlendi: arka uç hiç yoktur.
int vk_backend_available(void) { return 0; }
uint32_t vk_backend_supported(void) { return 0; }
const char* vk_backend_device_name(void) { return ""; }
static void unavailable(void) { marlin_rt_panic("Vulkan arka ucu bu calisma zamaninda yok"); }
void* vk_module_load(const void* key, const MarlinGpuPackage* package) {
    (void)key, (void)package;
    unavailable();
    return NULL;
}
void* vk_module_function(void* module, const char* name) {
    (void)module, (void)name;
    unavailable();
    return NULL;
}
void vk_launch(void* function, intptr_t gridX, intptr_t gridY, intptr_t gridZ, intptr_t blockX, intptr_t blockY,
               intptr_t blockZ, void** params, size_t paramsCount) {
    (void)function, (void)gridX, (void)gridY, (void)gridZ, (void)blockX, (void)blockY, (void)blockZ, (void)params,
        (void)paramsCount;
    unavailable();
}
void* vk_alloc(uint64_t size) {
    (void)size;
    unavailable();
    return NULL;
}
void vk_free(void* ptr) { (void)ptr; }
void vk_memcpy(void* dst, const void* src, size_t size) { memcpy(dst, src, size); }
void vk_memset32(void* dst, uint32_t value, size_t count) {
    (void)dst, (void)value, (void)count;
    unavailable();
}
void vk_memset16(void* dst, uint16_t value, size_t count) {
    (void)dst, (void)value, (void)count;
    unavailable();
}
void vk_synchronize(void) {}

#else

#define VK_NO_PROTOTYPES
#include <vulkan/vulkan_core.h>

#ifdef __APPLE__
int setenv(const char* name, const char* value, int overwrite); // (POSIX; -std=c11 gizleyebilir)
#endif

// ---------------------------------------------------------------------------
// Vulkan fonksiyonları (çalışırken yüklenir)
// ---------------------------------------------------------------------------

#define VK_GLOBAL_FUNCTIONS(X)                                                         \
    X(CreateInstance)                                                                  \
    X(EnumerateInstanceExtensionProperties)

#define VK_INSTANCE_FUNCTIONS(X)                                                       \
    X(EnumeratePhysicalDevices)                                                        \
    X(GetPhysicalDeviceProperties)                                                     \
    X(GetPhysicalDeviceFeatures)                                                       \
    X(GetPhysicalDeviceQueueFamilyProperties)                                          \
    X(GetPhysicalDeviceMemoryProperties)                                               \
    X(EnumerateDeviceExtensionProperties)                                              \
    X(CreateDevice)                                                                    \
    X(GetDeviceProcAddr)

#define VK_DEVICE_FUNCTIONS(X)                                                         \
    X(GetDeviceQueue)                                                                  \
    X(CreateBuffer)                                                                    \
    X(DestroyBuffer)                                                                   \
    X(GetBufferMemoryRequirements)                                                     \
    X(AllocateMemory)                                                                  \
    X(FreeMemory)                                                                      \
    X(BindBufferMemory)                                                                \
    X(MapMemory)                                                                       \
    X(CreateShaderModule)                                                              \
    X(CreateDescriptorSetLayout)                                                       \
    X(CreatePipelineLayout)                                                            \
    X(CreateComputePipelines)                                                          \
    X(CreateDescriptorPool)                                                            \
    X(ResetDescriptorPool)                                                             \
    X(AllocateDescriptorSets)                                                          \
    X(UpdateDescriptorSets)                                                            \
    X(CreateCommandPool)                                                               \
    X(AllocateCommandBuffers)                                                          \
    X(BeginCommandBuffer)                                                              \
    X(EndCommandBuffer)                                                                \
    X(ResetCommandBuffer)                                                              \
    X(CmdBindPipeline)                                                                 \
    X(CmdBindDescriptorSets)                                                           \
    X(CmdDispatch)                                                                     \
    X(CmdPipelineBarrier)                                                              \
    X(CreateFence)                                                                     \
    X(WaitForFences)                                                                   \
    X(ResetFences)                                                                     \
    X(QueueSubmit)

#define DECLARE_FUNCTION(name) PFN_vk##name name;
static struct {
    PFN_vkGetInstanceProcAddr GetInstanceProcAddr;
    PFN_vkEnumerateInstanceVersion EnumerateInstanceVersion; // Vulkan 1.1+
    PFN_vkCmdDispatchBase CmdDispatchBase;                   // Vulkan 1.1+
    VK_GLOBAL_FUNCTIONS(DECLARE_FUNCTION)
    VK_INSTANCE_FUNCTIONS(DECLARE_FUNCTION)
    VK_DEVICE_FUNCTIONS(DECLARE_FUNCTION)
} vk;
#undef DECLARE_FUNCTION

// ---------------------------------------------------------------------------
// Durum
// ---------------------------------------------------------------------------

typedef struct {
    VkBuffer buffer;
    VkDeviceMemory memory;
    unsigned char* data; // eşlenmiş adres
    VkDeviceSize size;
} Buffer;

typedef struct {
    char kind;   // 's' sayı, 'm' memref
    int rank;    // memref
    int bytes;   // sayının ya da elemanın boyutu
} KernelArg;

typedef struct Kernel {
    struct Kernel* next;
    char* name;
    int argCount;
    KernelArg* args;
    int slots;   // sayı yuvası sayısı
    int buffers; // memref sayısı (bağlama 1...)
    VkDescriptorSetLayout setLayout;
    VkPipelineLayout layout;
    VkPipeline pipeline;
} Kernel;

typedef struct Module {
    struct Module* next;
    const void* key;
    VkShaderModule shader;
    Kernel* kernels;
} Module;

#define ARENA_SIZE (1u << 20)      // bir gönderimdeki yuvalar
#define DESCRIPTOR_SETS 256        // bir gönderimdeki çekirdek başlatma
#define DESCRIPTORS_PER_SET 16

static struct {
    VkInstance instance;
    VkPhysicalDevice physical;
    VkDevice device;
    VkQueue queue;
    uint32_t family;
    VkPhysicalDeviceProperties properties;
    VkPhysicalDeviceMemoryProperties memory;
    uint32_t supported;
    int dispatchBase;

    mr_mutex lock;
    VkCommandPool commandPool;
    VkCommandBuffer commands;
    VkFence fence;
    VkDescriptorPool descriptors;
    int recording;
    int dispatches;   // komut tamponundaki çekirdek sayısı

    Buffer arena;     // yuvalar
    VkDeviceSize arenaUsed;
    VkDeviceSize arenaAlign;
    Buffer empty;     // boş memref'ler için

    Buffer* allocations;
    size_t allocationCount;
    size_t allocationCapacity;
    Module* modules;
} g;

// ---------------------------------------------------------------------------
// Yardımcılar
// ---------------------------------------------------------------------------

static void fatal(VkResult result, const char* call) {
    char message[160];
    snprintf(message, sizeof message, "Vulkan: '%s' basarisiz (VkResult %d)", call, (int)result);
    marlin_rt_panic(message);
}
#define CHECK(call)                                  \
    do {                                             \
        VkResult result_ = (call);                   \
        if (result_ != VK_SUCCESS) fatal(result_, #call); \
    } while (0)

static int has_extension(const VkExtensionProperties* list, uint32_t count, const char* name) {
    for (uint32_t i = 0; i < count; ++i)
        if (strcmp(list[i].extensionName, name) == 0) return 1;
    return 0;
}

// Vulkan kütüphanesi. MARLIN_VULKAN_LIBRARY (dosya yolu) her şeyden önce
// gelir. macOS'ta önce programın yanı: derleyici GPU kullanan programın
// yanına MoltenVK'yi koyar (bkz. LLVMBackend/NativeCodegen.cpp); sonra Vulkan
// SDK / Homebrew yükleyicisi, en son sisteme kurulu MoltenVK. MoltenVK
// yükleyici olmadan da kullanılabilir: Vulkan'ın giriş fonksiyonlarını
// kendisi dışa açar.
static void* open_loader(void) {
    char* chosen = mr_getenv("MARLIN_VULKAN_LIBRARY");
    if (chosen && *chosen) {
        void* library = mr_library_open(chosen);
        free(chosen);
        return library;
    }
    free(chosen);
#if defined(_WIN32)
    static const char* const names[] = {"vulkan-1.dll"};
#elif defined(__APPLE__)
    static const char* const names[] = {
        "@executable_path/libMoltenVK.dylib", "@executable_path/libvulkan.1.dylib",
        "libvulkan.1.dylib",                  "/usr/local/lib/libvulkan.1.dylib",
        "/opt/homebrew/lib/libvulkan.1.dylib", "libMoltenVK.dylib",
        "/usr/local/lib/libMoltenVK.dylib",   "/opt/homebrew/lib/libMoltenVK.dylib"};
#else
    static const char* const names[] = {"libvulkan.so.1", "libvulkan.so"};
#endif
    for (size_t i = 0; i < sizeof names / sizeof names[0]; ++i) {
        void* library = mr_library_open(names[i]);
        if (library) return library;
    }
    return NULL;
}

// Aygıtın türüne göre tercih: ayrık > tümleşik > sanal > yazılım (lavapipe).
static int device_score(VkPhysicalDeviceType type) {
    switch (type) {
    case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU: return 4;
    case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU: return 3;
    case VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU: return 2;
    case VK_PHYSICAL_DEVICE_TYPE_CPU: return 1;
    default: return 0;
    }
}

static int compute_family(VkPhysicalDevice device, uint32_t* family) {
    uint32_t count = 0;
    vk.GetPhysicalDeviceQueueFamilyProperties(device, &count, NULL);
    VkQueueFamilyProperties* families = calloc(count ? count : 1, sizeof *families);
    vk.GetPhysicalDeviceQueueFamilyProperties(device, &count, families);
    int found = 0;
    for (uint32_t i = 0; i < count && !found; ++i) {
        if (families[i].queueFlags & VK_QUEUE_COMPUTE_BIT) {
            *family = i;
            found = 1;
        }
    }
    free(families);
    return found;
}

static int find_memory_type(uint32_t typeBits, VkMemoryPropertyFlags required, uint32_t* index) {
    for (uint32_t i = 0; i < g.memory.memoryTypeCount; ++i) {
        if ((typeBits & (1u << i)) && (g.memory.memoryTypes[i].propertyFlags & required) == required) {
            *index = i;
            return 1;
        }
    }
    return 0;
}

// CPU'ya eşlenmiş bir depolama tamponu; açılamazsa 0.
static int create_buffer(VkDeviceSize size, Buffer* out) {
    if (size < 16) size = 16;
    VkBufferCreateInfo info = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    info.size = size;
    info.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    VkBuffer buffer;
    if (vk.CreateBuffer(g.device, &info, NULL, &buffer) != VK_SUCCESS) return 0;

    VkMemoryRequirements requirements;
    vk.GetBufferMemoryRequirements(g.device, buffer, &requirements);
    const VkMemoryPropertyFlags host = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    const VkMemoryPropertyFlags choices[2] = {host | VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, host};
    VkDeviceMemory memory = VK_NULL_HANDLE;
    for (int c = 0; c < 2 && memory == VK_NULL_HANDLE; ++c) {
        uint32_t type;
        if (!find_memory_type(requirements.memoryTypeBits, choices[c], &type)) continue;
        VkMemoryAllocateInfo allocate = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        allocate.allocationSize = requirements.size;
        allocate.memoryTypeIndex = type;
        if (vk.AllocateMemory(g.device, &allocate, NULL, &memory) != VK_SUCCESS) memory = VK_NULL_HANDLE;
    }
    if (memory == VK_NULL_HANDLE) {
        vk.DestroyBuffer(g.device, buffer, NULL);
        return 0;
    }
    void* data = NULL;
    if (vk.BindBufferMemory(g.device, buffer, memory, 0) != VK_SUCCESS ||
        vk.MapMemory(g.device, memory, 0, VK_WHOLE_SIZE, 0, &data) != VK_SUCCESS) {
        vk.DestroyBuffer(g.device, buffer, NULL);
        vk.FreeMemory(g.device, memory, NULL);
        return 0;
    }
    out->buffer = buffer;
    out->memory = memory;
    out->data = data;
    out->size = size;
    return 1;
}

static void destroy_buffer(Buffer* buffer) {
    vk.DestroyBuffer(g.device, buffer->buffer, NULL);
    vk.FreeMemory(g.device, buffer->memory, NULL); // (eşleme de kalkar)
}

// ---------------------------------------------------------------------------
// Başlatma (bir kez; marlin_gpu.c'nin kararı sırasında)
// ---------------------------------------------------------------------------

static int create_instance(void) {
    void* library = open_loader();
    if (!library) return 0;
    vk.GetInstanceProcAddr = (PFN_vkGetInstanceProcAddr)mr_library_symbol(library, "vkGetInstanceProcAddr");
    if (!vk.GetInstanceProcAddr) return 0;
#define LOAD_GLOBAL(name)                                                         \
    vk.name = (PFN_vk##name)vk.GetInstanceProcAddr(VK_NULL_HANDLE, "vk" #name); \
    if (!vk.name) return 0;
    VK_GLOBAL_FUNCTIONS(LOAD_GLOBAL)
#undef LOAD_GLOBAL
    vk.EnumerateInstanceVersion =
        (PFN_vkEnumerateInstanceVersion)vk.GetInstanceProcAddr(VK_NULL_HANDLE, "vkEnumerateInstanceVersion");

    uint32_t version = VK_API_VERSION_1_0;
    if (vk.EnumerateInstanceVersion) vk.EnumerateInstanceVersion(&version);

    // macOS: MoltenVK "taşınabilirlik" aygıtıdır; yükleyici onu ancak
    // istenirse listeler.
    uint32_t count = 0;
    vk.EnumerateInstanceExtensionProperties(NULL, &count, NULL);
    VkExtensionProperties* available = calloc(count ? count : 1, sizeof *available);
    vk.EnumerateInstanceExtensionProperties(NULL, &count, available);
    const char* extensions[1];
    uint32_t extensionCount = 0;
    VkInstanceCreateFlags flags = 0;
#ifdef VK_KHR_portability_enumeration
    if (has_extension(available, count, VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME)) {
        extensions[extensionCount++] = VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME;
        flags |= VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR;
    }
#endif
    free(available);

    VkApplicationInfo app = {VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = "marlin";
    app.pEngineName = "marlin";
    app.apiVersion = version >= VK_API_VERSION_1_1 ? VK_API_VERSION_1_1 : VK_API_VERSION_1_0;
    VkInstanceCreateInfo info = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    info.flags = flags;
    info.pApplicationInfo = &app;
    info.enabledExtensionCount = extensionCount;
    info.ppEnabledExtensionNames = extensions;
    if (vk.CreateInstance(&info, NULL, &g.instance) != VK_SUCCESS) return 0;

#define LOAD_INSTANCE(name)                                                    \
    vk.name = (PFN_vk##name)vk.GetInstanceProcAddr(g.instance, "vk" #name); \
    if (!vk.name) return 0;
    VK_INSTANCE_FUNCTIONS(LOAD_INSTANCE)
#undef LOAD_INSTANCE
    return 1;
}

static int pick_device(void) {
    uint32_t count = 0;
    if (vk.EnumeratePhysicalDevices(g.instance, &count, NULL) != VK_SUCCESS || count == 0) return 0;
    VkPhysicalDevice* devices = calloc(count, sizeof *devices);
    vk.EnumeratePhysicalDevices(g.instance, &count, devices);
    int best = -1;
    for (uint32_t i = 0; i < count; ++i) {
        VkPhysicalDeviceProperties properties;
        vk.GetPhysicalDeviceProperties(devices[i], &properties);
        uint32_t family;
        if (!compute_family(devices[i], &family)) continue;
        const int score = device_score(properties.deviceType);
        if (score > best) {
            best = score;
            g.physical = devices[i];
            g.family = family;
            g.properties = properties;
        }
    }
    free(devices);
    return best >= 0;
}

static int create_device(void) {
    VkPhysicalDeviceFeatures features;
    vk.GetPhysicalDeviceFeatures(g.physical, &features);
    VkPhysicalDeviceFeatures enabled;
    memset(&enabled, 0, sizeof enabled);
    enabled.shaderFloat64 = features.shaderFloat64;
    enabled.shaderInt64 = features.shaderInt64;

    // Testler için: aygıtı f64'süz say (Apple GPU'ları, Intel Arc gibi);
    // f64 kullanan @GPU fonksiyonlar CPU'ya düşmeli.
    char* noFloat64 = mr_getenv("MARLIN_VULKAN_NO_FLOAT64");
    if (noFloat64 && strcmp(noFloat64, "1") == 0) features.shaderFloat64 = VK_FALSE;
    free(noFloat64);
    enabled.shaderFloat64 = features.shaderFloat64;

    const VkPhysicalDeviceLimits* limits = &g.properties.limits;
    if (features.shaderFloat64) g.supported |= MARLIN_GPU_NEEDS_FLOAT64;
    if (features.shaderInt64) g.supported |= MARLIN_GPU_NEEDS_INT64;
    if (limits->maxComputeWorkGroupInvocations >= 256 && limits->maxComputeWorkGroupSize[0] >= 256 &&
        limits->maxComputeWorkGroupSize[1] >= 16)
        g.supported |= MARLIN_GPU_NEEDS_WIDE_GROUP;

    uint32_t count = 0;
    vk.EnumerateDeviceExtensionProperties(g.physical, NULL, &count, NULL);
    VkExtensionProperties* available = calloc(count ? count : 1, sizeof *available);
    vk.EnumerateDeviceExtensionProperties(g.physical, NULL, &count, available);
    const char* extensions[2];
    uint32_t extensionCount = 0;
    // SPIR-V çekirdekleri StorageBuffer sınıfını kullanır (Vulkan 1.1'de çekirdek).
    if (has_extension(available, count, "VK_KHR_storage_buffer_storage_class"))
        extensions[extensionCount++] = "VK_KHR_storage_buffer_storage_class";
    // Taşınabilirlik aygıtı (MoltenVK) bu eki listeliyorsa açılması zorunludur.
    if (has_extension(available, count, "VK_KHR_portability_subset"))
        extensions[extensionCount++] = "VK_KHR_portability_subset";
    free(available);

    const float priority = 1.0f;
    VkDeviceQueueCreateInfo queue = {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    queue.queueFamilyIndex = g.family;
    queue.queueCount = 1;
    queue.pQueuePriorities = &priority;
    VkDeviceCreateInfo info = {VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    info.queueCreateInfoCount = 1;
    info.pQueueCreateInfos = &queue;
    info.enabledExtensionCount = extensionCount;
    info.ppEnabledExtensionNames = extensions;
    info.pEnabledFeatures = &enabled;
    if (vk.CreateDevice(g.physical, &info, NULL, &g.device) != VK_SUCCESS) return 0;

#define LOAD_DEVICE(name)                                                  \
    vk.name = (PFN_vk##name)vk.GetDeviceProcAddr(g.device, "vk" #name); \
    if (!vk.name) return 0;
    VK_DEVICE_FUNCTIONS(LOAD_DEVICE)
#undef LOAD_DEVICE
    if (g.properties.apiVersion >= VK_API_VERSION_1_1)
        vk.CmdDispatchBase = (PFN_vkCmdDispatchBase)vk.GetDeviceProcAddr(g.device, "vkCmdDispatchBase");
    g.dispatchBase = vk.CmdDispatchBase != NULL;
    vk.GetDeviceQueue(g.device, g.family, 0, &g.queue);
    vk.GetPhysicalDeviceMemoryProperties(g.physical, &g.memory);
    return 1;
}

static int create_submission_state(void) {
    VkCommandPoolCreateInfo pool = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pool.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pool.queueFamilyIndex = g.family;
    if (vk.CreateCommandPool(g.device, &pool, NULL, &g.commandPool) != VK_SUCCESS) return 0;
    VkCommandBufferAllocateInfo allocate = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    allocate.commandPool = g.commandPool;
    allocate.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocate.commandBufferCount = 1;
    if (vk.AllocateCommandBuffers(g.device, &allocate, &g.commands) != VK_SUCCESS) return 0;
    VkFenceCreateInfo fence = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    if (vk.CreateFence(g.device, &fence, NULL, &g.fence) != VK_SUCCESS) return 0;

    VkDescriptorPoolSize size = {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, DESCRIPTOR_SETS * DESCRIPTORS_PER_SET};
    VkDescriptorPoolCreateInfo descriptors = {VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    descriptors.maxSets = DESCRIPTOR_SETS;
    descriptors.poolSizeCount = 1;
    descriptors.pPoolSizes = &size;
    if (vk.CreateDescriptorPool(g.device, &descriptors, NULL, &g.descriptors) != VK_SUCCESS) return 0;

    if (!create_buffer(ARENA_SIZE, &g.arena) || !create_buffer(16, &g.empty)) return 0;
    g.arenaAlign = g.properties.limits.minStorageBufferOffsetAlignment;
    if (g.arenaAlign < 16) g.arenaAlign = 16;
    return 1;
}

static int detect_unguarded(void) {
    return create_instance() && pick_device() && create_device() && create_submission_state();
}

static int detect(void) {
    mr_mutex_init(&g.lock);
#ifdef __APPLE__
    // MoltenVK varsayılan olarak her açılışta stderr'e onlarca bilgi satırı
    // yazar; kullanıcı istemedikçe yalnızca hatalar.
    setenv("MVK_CONFIG_LOG_LEVEL", "1", /*overwrite=*/0);
#endif
    // Sürücünün başlatılırken çökmesi programı düşürmesin (bkz. platform.h).
    return mr_call_guarded(detect_unguarded);
}

// 0 bilinmiyor, 1 karar veriliyor, 2 yok, 3 var (marlin_gpu.c'deki gibi).
static int state = 0;

int vk_backend_available(void) {
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

uint32_t vk_backend_supported(void) { return vk_backend_available() ? g.supported : 0; }

const char* vk_backend_device_name(void) { return vk_backend_available() ? g.properties.deviceName : ""; }

// ---------------------------------------------------------------------------
// Gönderim
// ---------------------------------------------------------------------------

// Biriken komutları gönderir ve bitmesini bekler; ardından gönderime ait
// kaynaklar (yuvalar, bağlama kümeleri) yeniden kullanılabilir. Kilit altında.
static void flush(void) {
    if (!g.recording) return;
    // Çekirdeklerin yazdıkları CPU'ya görünür olsun.
    VkMemoryBarrier barrier = {VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    vk.CmdPipelineBarrier(g.commands, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &barrier,
                          0, NULL, 0, NULL);
    CHECK(vk.EndCommandBuffer(g.commands));
    VkSubmitInfo submit = {VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &g.commands;
    CHECK(vk.QueueSubmit(g.queue, 1, &submit, g.fence));
    CHECK(vk.WaitForFences(g.device, 1, &g.fence, VK_TRUE, UINT64_MAX));
    CHECK(vk.ResetFences(g.device, 1, &g.fence));
    CHECK(vk.ResetCommandBuffer(g.commands, 0));
    CHECK(vk.ResetDescriptorPool(g.device, g.descriptors, 0));
    g.arenaUsed = 0;
    g.recording = 0;
    g.dispatches = 0;
}

void vk_synchronize(void) {
    if (!vk_backend_available()) return;
    mr_mutex_lock(&g.lock);
    flush();
    mr_mutex_unlock(&g.lock);
}

// ---------------------------------------------------------------------------
// Bellek
// ---------------------------------------------------------------------------

void* vk_alloc(uint64_t size) {
    if (size == 0) return NULL;
    // Çekirdek tamponun tamamını görür; aygıtın tek bağlamada izin verdiği
    // boyuttan büyüğü (lavapipe 128 MB, kartlarda genelde 1-4 GB)
    // okunamazdı.
    if (size > g.properties.limits.maxStorageBufferRange) {
        char message[160];
        snprintf(message, sizeof message, "Vulkan: %llu baytlik GPU tamponu aygitin sinirini (%u bayt) asiyor",
                 (unsigned long long)size, g.properties.limits.maxStorageBufferRange);
        marlin_rt_panic(message);
    }
    Buffer buffer;
    mr_mutex_lock(&g.lock);
    if (!create_buffer(size, &buffer)) {
        mr_mutex_unlock(&g.lock);
        marlin_rt_panic("Vulkan: GPU bellegi ayrilamadi");
    }
    if (g.allocationCount == g.allocationCapacity) {
        g.allocationCapacity = g.allocationCapacity ? 2 * g.allocationCapacity : 16;
        g.allocations = realloc(g.allocations, g.allocationCapacity * sizeof *g.allocations);
        if (!g.allocations) marlin_rt_panic("bellek yetersiz");
    }
    g.allocations[g.allocationCount++] = buffer;
    mr_mutex_unlock(&g.lock);
    return buffer.data;
}

// Adresi içeren tampon (kilit altında); yoksa NULL. Tamponlar bellekte art
// arda durabilir: bir tamponun sonu sonrakinin başıdır. Sona eşit adres
// (boş görünüm) ancak adresi içeren başka tampon yoksa o tampona aittir.
static Buffer* find_buffer(const void* ptr) {
    const unsigned char* p = ptr;
    Buffer* atEnd = NULL;
    for (size_t i = 0; i < g.allocationCount; ++i) {
        Buffer* b = &g.allocations[i];
        if (p >= b->data && p < b->data + b->size) return b;
        if (p == b->data + b->size) atEnd = b;
    }
    return atEnd;
}

void vk_free(void* ptr) {
    if (!ptr) return;
    mr_mutex_lock(&g.lock);
    flush(); // tamponu kullanan iş bitmiş olmalı
    for (size_t i = 0; i < g.allocationCount; ++i) {
        if (g.allocations[i].data == ptr) {
            destroy_buffer(&g.allocations[i]);
            g.allocations[i] = g.allocations[--g.allocationCount];
            break;
        }
    }
    mr_mutex_unlock(&g.lock);
}

void vk_memcpy(void* dst, const void* src, size_t size) {
    mr_mutex_lock(&g.lock);
    flush();
    mr_mutex_unlock(&g.lock);
    if (size) memcpy(dst, src, size);
}

void vk_memset32(void* dst, uint32_t value, size_t count) {
    vk_synchronize();
    uint32_t* p = dst;
    for (size_t i = 0; i < count; ++i) p[i] = value;
}

void vk_memset16(void* dst, uint16_t value, size_t count) {
    vk_synchronize();
    uint16_t* p = dst;
    for (size_t i = 0; i < count; ++i) p[i] = value;
}

// ---------------------------------------------------------------------------
// Çekirdekler
// ---------------------------------------------------------------------------

static char* copy_text(const char* begin, size_t length) {
    char* text = malloc(length + 1);
    if (!text) marlin_rt_panic("bellek yetersiz");
    memcpy(text, begin, length);
    text[length] = '\0';
    return text;
}

// Tablonun bir satırı: "<ad> <argüman sayısı> s8 m2x8 ..." (gpu_package.h).
static Kernel* parse_kernel(const char* line, const char* end) {
    Kernel* kernel = calloc(1, sizeof *kernel);
    if (!kernel) marlin_rt_panic("bellek yetersiz");
    const char* p = line;
    while (p < end && *p != ' ') ++p;
    kernel->name = copy_text(line, (size_t)(p - line));
    kernel->argCount = (int)strtol(p, (char**)&p, 10);
    kernel->args = calloc((size_t)(kernel->argCount ? kernel->argCount : 1), sizeof *kernel->args);
    for (int i = 0; i < kernel->argCount; ++i) {
        while (p < end && *p == ' ') ++p;
        KernelArg* arg = &kernel->args[i];
        arg->kind = *p++;
        if (arg->kind == 'm') {
            arg->rank = (int)strtol(p, (char**)&p, 10);
            ++p; // 'x'
            arg->bytes = (int)strtol(p, (char**)&p, 10);
            kernel->slots += 1 + 2 * arg->rank;
            kernel->buffers += 1;
        } else {
            arg->bytes = (int)strtol(p, (char**)&p, 10);
            kernel->slots += 1;
        }
    }
    return kernel;
}

void* vk_module_load(const void* key, const MarlinGpuPackage* package) {
    mr_mutex_lock(&g.lock);
    for (Module* m = g.modules; m; m = m->next) {
        if (m->key == key) {
            mr_mutex_unlock(&g.lock);
            return m;
        }
    }
    if (package->spirv_size == 0) {
        mr_mutex_unlock(&g.lock);
        marlin_rt_panic("Vulkan: cekirdegin SPIR-V surumu yok");
    }
    Module* module = calloc(1, sizeof *module);
    if (!module) marlin_rt_panic("bellek yetersiz");
    module->key = key;

    // SPIR-V sözcükleri 4 bayta hizalı olmalı; programdaki kopya öyle olmayabilir.
    uint32_t* code = malloc(package->spirv_size);
    if (!code) marlin_rt_panic("bellek yetersiz");
    memcpy(code, package->spirv, package->spirv_size);
    VkShaderModuleCreateInfo info = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    info.codeSize = package->spirv_size;
    info.pCode = code;
    CHECK(vk.CreateShaderModule(g.device, &info, NULL, &module->shader));
    free(code);

    for (const char* line = package->table; *line;) {
        const char* end = strchr(line, '\n');
        if (!end) end = line + strlen(line);
        if (end > line) {
            Kernel* kernel = parse_kernel(line, end);
            kernel->next = module->kernels;
            module->kernels = kernel;
        }
        line = *end ? end + 1 : end;
    }
    module->next = g.modules;
    g.modules = module;
    mr_mutex_unlock(&g.lock);
    return module;
}

static void create_pipeline(Module* module, Kernel* kernel) {
    const uint32_t bindings = (uint32_t)(1 + kernel->buffers);
    VkDescriptorSetLayoutBinding* binding = calloc(bindings, sizeof *binding);
    if (!binding) marlin_rt_panic("bellek yetersiz");
    for (uint32_t i = 0; i < bindings; ++i) {
        binding[i].binding = i;
        binding[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        binding[i].descriptorCount = 1;
        binding[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    }
    VkDescriptorSetLayoutCreateInfo set = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    set.bindingCount = bindings;
    set.pBindings = binding;
    CHECK(vk.CreateDescriptorSetLayout(g.device, &set, NULL, &kernel->setLayout));
    free(binding);

    VkPipelineLayoutCreateInfo layout = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    layout.setLayoutCount = 1;
    layout.pSetLayouts = &kernel->setLayout;
    CHECK(vk.CreatePipelineLayout(g.device, &layout, NULL, &kernel->layout));

    VkComputePipelineCreateInfo pipeline = {VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    pipeline.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    pipeline.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    pipeline.stage.module = module->shader;
    pipeline.stage.pName = kernel->name;
    pipeline.layout = kernel->layout;
    if (g.dispatchBase) pipeline.flags = VK_PIPELINE_CREATE_DISPATCH_BASE_BIT;
    CHECK(vk.CreateComputePipelines(g.device, VK_NULL_HANDLE, 1, &pipeline, NULL, &kernel->pipeline));
}

void* vk_module_function(void* handle, const char* name) {
    Module* module = handle;
    mr_mutex_lock(&g.lock);
    Kernel* kernel = module->kernels;
    while (kernel && strcmp(kernel->name, name) != 0) kernel = kernel->next;
    if (!kernel) {
        mr_mutex_unlock(&g.lock);
        marlin_rt_panic("Vulkan: cekirdek pakette yok");
    }
    if (kernel->pipeline == VK_NULL_HANDLE) create_pipeline(module, kernel);
    mr_mutex_unlock(&g.lock);
    return kernel;
}

static void begin_recording(void) {
    if (g.recording) return;
    VkCommandBufferBeginInfo begin = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    CHECK(vk.BeginCommandBuffer(g.commands, &begin));
    g.recording = 1;
}

void vk_launch(void* function, intptr_t gridX, intptr_t gridY, intptr_t gridZ, intptr_t blockX, intptr_t blockY,
               intptr_t blockZ, void** params, size_t paramsCount) {
    (void)blockX, (void)blockY, (void)blockZ; // blok boyutu çekirdeğin içinde (LocalSize)
    Kernel* kernel = function;
    if (gridX <= 0 || gridY <= 0 || gridZ <= 0) return;

    mr_mutex_lock(&g.lock);
    const VkDeviceSize slotBytes = (VkDeviceSize)(kernel->slots ? kernel->slots : 1) * 8;
    VkDeviceSize slotOffset = (g.arenaUsed + g.arenaAlign - 1) / g.arenaAlign * g.arenaAlign;
    if (g.dispatches >= DESCRIPTOR_SETS || kernel->buffers + 1 > DESCRIPTORS_PER_SET ||
        slotOffset + slotBytes > ARENA_SIZE) {
        flush();
        slotOffset = 0;
        if (kernel->buffers + 1 > DESCRIPTORS_PER_SET) {
            mr_mutex_unlock(&g.lock);
            marlin_rt_panic("Vulkan: cekirdegin tampon sayisi cok fazla");
        }
    }
    g.arenaUsed = slotOffset + slotBytes;

    // Parametreler (gpu-to-llvm'in sırası) → yuvalar ve tamponlar.
    uint64_t* slots = (uint64_t*)(g.arena.data + slotOffset);
    VkDescriptorBufferInfo buffers[DESCRIPTORS_PER_SET];
    buffers[0].buffer = g.arena.buffer;
    buffers[0].offset = slotOffset;
    buffers[0].range = slotBytes;
    size_t p = 0;
    int slot = 0, bufferIndex = 1;
    for (int i = 0; i < kernel->argCount; ++i) {
        const KernelArg* arg = &kernel->args[i];
        if (arg->kind == 's') {
            uint64_t value = 0;
            memcpy(&value, params[p++], (size_t)arg->bytes);
            slots[slot++] = value;
            continue;
        }
        unsigned char* aligned = *(unsigned char**)params[p + 1];
        const int64_t offset = *(int64_t*)params[p + 2];
        const Buffer* owner = aligned ? find_buffer(aligned) : &g.empty;
        if (!owner) {
            mr_mutex_unlock(&g.lock);
            marlin_rt_panic("Vulkan: cekirdege GPU'da olmayan bellek verildi");
        }
        const int64_t base = aligned ? (int64_t)(aligned - owner->data) / arg->bytes : 0;
        slots[slot++] = (uint64_t)(base + offset);
        for (int d = 0; d < arg->rank; ++d) slots[slot++] = (uint64_t)*(int64_t*)params[p + 3 + d];
        for (int d = 0; d < arg->rank; ++d) slots[slot++] = (uint64_t)*(int64_t*)params[p + 3 + arg->rank + d];
        p += 3 + 2 * (size_t)arg->rank;
        buffers[bufferIndex].buffer = owner->buffer;
        buffers[bufferIndex].offset = 0;
        buffers[bufferIndex].range = VK_WHOLE_SIZE;
        ++bufferIndex;
    }
    if (p != paramsCount) {
        mr_mutex_unlock(&g.lock);
        marlin_rt_panic("Vulkan: cekirdek parametreleri tabloyla uyusmuyor");
    }

    VkDescriptorSet set;
    VkDescriptorSetAllocateInfo allocate = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    allocate.descriptorPool = g.descriptors;
    allocate.descriptorSetCount = 1;
    allocate.pSetLayouts = &kernel->setLayout;
    CHECK(vk.AllocateDescriptorSets(g.device, &allocate, &set));
    VkWriteDescriptorSet writes[DESCRIPTORS_PER_SET];
    for (int i = 0; i < bufferIndex; ++i) {
        VkWriteDescriptorSet w = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        w.dstSet = set;
        w.dstBinding = (uint32_t)i;
        w.descriptorCount = 1;
        w.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        w.pBufferInfo = &buffers[i];
        writes[i] = w;
    }
    vk.UpdateDescriptorSets(g.device, (uint32_t)bufferIndex, writes, 0, NULL);

    begin_recording();
    if (g.dispatches > 0) {
        // Önceki çekirdeğin yazdıkları bu çekirdeğe görünür olsun.
        VkMemoryBarrier barrier = {VK_STRUCTURE_TYPE_MEMORY_BARRIER};
        barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
        vk.CmdPipelineBarrier(g.commands, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0,
                              1, &barrier, 0, NULL, 0, NULL);
    }
    vk.CmdBindPipeline(g.commands, VK_PIPELINE_BIND_POINT_COMPUTE, kernel->pipeline);
    vk.CmdBindDescriptorSets(g.commands, VK_PIPELINE_BIND_POINT_COMPUTE, kernel->layout, 0, 1, &set, 0, NULL);

    // Aygıtın tek seferde başlatabileceği blok sayısı sınırlıdır (en az
    // 65535); fazlası parçalara bölünür (blok numaraları kaydırılarak:
    // vkCmdDispatchBase, Vulkan 1.1).
    const uint32_t* maxCount = g.properties.limits.maxComputeWorkGroupCount;
    if ((uint64_t)gridX <= maxCount[0] && (uint64_t)gridY <= maxCount[1] && (uint64_t)gridZ <= maxCount[2]) {
        vk.CmdDispatch(g.commands, (uint32_t)gridX, (uint32_t)gridY, (uint32_t)gridZ);
    } else if (g.dispatchBase && (uint64_t)gridY <= maxCount[1] && (uint64_t)gridZ <= maxCount[2]) {
        for (uint64_t x = 0; x < (uint64_t)gridX; x += maxCount[0]) {
            uint64_t count = (uint64_t)gridX - x;
            if (count > maxCount[0]) count = maxCount[0];
            vk.CmdDispatchBase(g.commands, (uint32_t)x, 0, 0, (uint32_t)count, (uint32_t)gridY, (uint32_t)gridZ);
        }
    } else {
        mr_mutex_unlock(&g.lock);
        marlin_rt_panic("Vulkan: cekirdegin blok sayisi aygitin sinirini asiyor");
    }
    ++g.dispatches;
    mr_mutex_unlock(&g.lock);
}

#endif // MARLIN_HAVE_VULKAN
