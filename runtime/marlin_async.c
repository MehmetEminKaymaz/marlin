// MLIR async dialect'inin çalışma zamanı: paralel döngülerin ve görevlerin
// (marl / await) iş parçacığı havuzu ve bitişi bildiren nesneleri.
//
// MLIR'in kendi uygulaması (mlir/lib/ExecutionEngine/AsyncRuntime.cpp,
// libmlir_async_runtime) paylaşımlı bir C++ kütüphanesidir ve LLVM'e bağlıdır;
// onu kullanan program o kütüphane olmadan açılmaz, yani başka bir makineye
// tek başına taşınamaz. Bu dosya aynı C arayüzünü (AsyncRuntime.h) C11 ve
// platform.h ile gerçekleştirir; program yalnızca libmarlinrt'ye bağlanır.
// Davranış MLIR'inkiyle aynıdır:
//
//   * Havuz: işlemci sayısı kadar iş parçacığı (ilk işte başlatılır), sıra
//     FIFO. Bekleyen (engelleyen) await o iş parçacığını tutar; iç içe paralel
//     döngüler bu yüzden derleyicide sıralılaştırılır (NestedParallel).
//   * Token / value: başvuru sayılır, 2 başvuruyla doğar (biri çağırana, biri
//     işi bitirecek göreve). Hazır olunca bekleyenler (awaiters) nesnenin
//     kilidi tutulurken çalıştırılır.
//   * Group: içindeki token'ların hepsi hazır olunca hazırdır.
//
// Havuzun iş parçacıkları süreç bitene kadar yaşar; süreç biterken beklenmez
// (MLIR'inki beklerdi, bkz. platform.h mr_exit_now).

#include "marlin_rt.h"
#include "platform.h"

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

typedef void (*CoroResume)(void*);

enum { STATE_UNAVAILABLE = 0, STATE_AVAILABLE = 1, STATE_ERROR = 2 };
enum { KIND_EVENT, KIND_GROUP };

struct AsyncGroup;
struct AsyncEvent;

// Nesne hazır olunca yapılacak iş: bir coroutine'i sürdürmek (resume) ya da
// token'ın hazır olduğunu grubuna bildirmek.
typedef struct {
    CoroResume resume;  // NULL değilse: resume(handle)
    void* handle;
    struct AsyncGroup* group;
    struct AsyncEvent* token;
} Awaiter;

typedef struct {
    mr_mutex mu;
    mr_cond cv;
    Awaiter* list;
    size_t count, capacity;
} Waiters;

typedef struct {
    int64_t refs;
    int32_t kind;
} RefCounted;

// async.token (storage NULL) ve async.value.
typedef struct AsyncEvent {
    RefCounted rc;
    int8_t state;
    Waiters waiters;
    int8_t* storage;
} AsyncEvent;

typedef struct AsyncGroup {
    RefCounted rc;
    int64_t pending;  // hazır olmayı bekleyen token sayısı
    int64_t errors;
    int64_t rank;
    Waiters waiters;
} AsyncGroup;

static void* allocate(size_t size) {
    void* p = calloc(1, size);
    if (!p) marlin_rt_panic("async calisma zamani: bellek ayrilamadi");
    return p;
}

static void waiters_init(Waiters* w) {
    mr_mutex_init(&w->mu);
    mr_cond_init(&w->cv);
}

static void waiters_destroy(Waiters* w) {
    mr_mutex_destroy(&w->mu);
    mr_cond_destroy(&w->cv);
    free(w->list);
}

// (Kilit tutulurken çağrılır.)
static void waiters_push(Waiters* w, Awaiter awaiter) {
    if (w->count == w->capacity) {
        w->capacity = w->capacity ? w->capacity * 2 : 4;
        w->list = realloc(w->list, w->capacity * sizeof *w->list);
        if (!w->list) marlin_rt_panic("async calisma zamani: bellek ayrilamadi");
    }
    w->list[w->count++] = awaiter;
}

static void run_awaiter(const Awaiter* awaiter);

// (Kilit tutulurken çağrılır.) Bekleyenler bir kez çalışır.
static void waiters_run(Waiters* w) {
    for (size_t i = 0; i < w->count; ++i) run_awaiter(&w->list[i]);
    w->count = 0;
}

// ---------------------------------------------------------------------------
// İş parçacığı havuzu
// ---------------------------------------------------------------------------

typedef struct {
    CoroResume resume;
    void* handle;
} Job;

static struct {
    mr_mutex mu;
    mr_cond cv;
    Job* jobs;  // halka: [head, head + count)
    size_t head, count, capacity;
} pool;

static int pool_state;  // 0: yok, 1: başlatılıyor, 2: hazır

static int worker_count(void) {
    static int count;
    int n = __atomic_load_n(&count, __ATOMIC_RELAXED);
    if (n == 0) {
        n = mr_cpu_count();
        __atomic_store_n(&count, n, __ATOMIC_RELAXED);
    }
    return n;
}

static void worker_main(void* unused) {
    (void)unused;
    for (;;) {
        mr_mutex_lock(&pool.mu);
        while (pool.count == 0) mr_cond_wait(&pool.cv, &pool.mu);
        const Job job = pool.jobs[pool.head];
        pool.head = (pool.head + 1) % pool.capacity;
        pool.count--;
        mr_mutex_unlock(&pool.mu);
        job.resume(job.handle);
    }
}

static void pool_start(void) {
    if (__atomic_load_n(&pool_state, __ATOMIC_ACQUIRE) == 2) return;
    int expected = 0;
    if (!__atomic_compare_exchange_n(&pool_state, &expected, 1, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
        // Başka bir iş parçacığı başlatıyor (kısa sürer).
        while (__atomic_load_n(&pool_state, __ATOMIC_ACQUIRE) != 2) {}
        return;
    }
    mr_mutex_init(&pool.mu);
    mr_cond_init(&pool.cv);
    pool.capacity = 64;
    pool.jobs = allocate(pool.capacity * sizeof *pool.jobs);
    const int n = worker_count();
    for (int i = 0; i < n; ++i)
        if (!mr_thread_start(worker_main, NULL)) marlin_rt_panic("async calisma zamani: is parcacigi baslatilamadi");
    __atomic_store_n(&pool_state, 2, __ATOMIC_RELEASE);
}

static void pool_submit(CoroResume resume, void* handle) {
    pool_start();
    mr_mutex_lock(&pool.mu);
    if (pool.count == pool.capacity) {
        // Halka büyütülürken sıra korunur: elemanlar yeni dizinin başına açılır.
        Job* jobs = allocate(pool.capacity * 2 * sizeof *jobs);
        for (size_t i = 0; i < pool.count; ++i) jobs[i] = pool.jobs[(pool.head + i) % pool.capacity];
        free(pool.jobs);
        pool.jobs = jobs;
        pool.head = 0;
        pool.capacity *= 2;
    }
    pool.jobs[(pool.head + pool.count) % pool.capacity] = (Job){resume, handle};
    pool.count++;
    mr_cond_signal(&pool.cv);
    mr_mutex_unlock(&pool.mu);
}

// ---------------------------------------------------------------------------
// Başvuru sayımı
// ---------------------------------------------------------------------------

static void destroy(RefCounted* object) {
    if (object->kind == KIND_GROUP) {
        AsyncGroup* group = (AsyncGroup*)object;
        waiters_destroy(&group->waiters);
    } else {
        AsyncEvent* event = (AsyncEvent*)object;
        waiters_destroy(&event->waiters);
        free(event->storage);
    }
    free(object);
}

void mlirAsyncRuntimeAddRef(void* object, int64_t count) {
    __atomic_add_fetch(&((RefCounted*)object)->refs, count, __ATOMIC_RELAXED);
}

void mlirAsyncRuntimeDropRef(void* object, int64_t count) {
    RefCounted* rc = object;
    const int64_t previous = __atomic_fetch_sub(&rc->refs, count, __ATOMIC_ACQ_REL);
    if (previous < count) marlin_rt_panic("async calisma zamani: basvuru sayaci sifirin altina indi");
    if (previous == count) destroy(rc);
}

// ---------------------------------------------------------------------------
// Token, value, group
// ---------------------------------------------------------------------------

static AsyncEvent* event_new(int64_t storage_size) {
    AsyncEvent* event = allocate(sizeof *event);
    event->rc.refs = 2;  // çağıran + işi bitirecek görev
    event->rc.kind = KIND_EVENT;
    event->state = STATE_UNAVAILABLE;
    waiters_init(&event->waiters);
    if (storage_size >= 0) event->storage = allocate(storage_size > 0 ? (size_t)storage_size : 1);
    return event;
}

void* mlirAsyncRuntimeCreateToken(void) { return event_new(-1); }
void* mlirAsyncRuntimeCreateValue(int64_t size) { return event_new(size); }

void* mlirAsyncRuntimeCreateGroup(int64_t size) {
    AsyncGroup* group = allocate(sizeof *group);
    group->rc.refs = 1;
    group->rc.kind = KIND_GROUP;
    group->pending = size;
    waiters_init(&group->waiters);
    return group;
}

static int8_t state_of(const AsyncEvent* event) { return __atomic_load_n(&event->state, __ATOMIC_ACQUIRE); }

// Token hazır: grubun sayacını azalt; son token ise grubu bekleyenleri
// çalıştır. (Grubun kilidi tutulurken çağrılır.)
static void group_token_ready(AsyncGroup* group, AsyncEvent* token) {
    if (state_of(token) == STATE_ERROR) __atomic_add_fetch(&group->errors, 1, __ATOMIC_RELAXED);
    const int64_t previous = __atomic_fetch_sub(&group->pending, 1, __ATOMIC_ACQ_REL);
    if (previous <= 0) marlin_rt_panic("async calisma zamani: gruba boyutundan fazla token eklendi");
    if (previous == 1) {
        mr_cond_broadcast(&group->waiters.cv);
        waiters_run(&group->waiters);
    }
}

static void run_awaiter(const Awaiter* awaiter) {
    if (awaiter->resume) {
        awaiter->resume(awaiter->handle);
        return;
    }
    AsyncGroup* group = awaiter->group;
    mr_mutex_lock(&group->waiters.mu);
    group_token_ready(group, awaiter->token);
    mr_mutex_unlock(&group->waiters.mu);
    // (Kilit bırakıldıktan sonra: son başvuruysa grup burada yok olur.)
    mlirAsyncRuntimeDropRef(group, 1);
}

int64_t mlirAsyncRuntimeAddTokenToGroup(void* tokenObject, void* groupObject) {
    AsyncEvent* token = tokenObject;
    AsyncGroup* group = groupObject;
    mr_mutex_lock(&token->waiters.mu);
    mr_mutex_lock(&group->waiters.mu);
    const int64_t rank = __atomic_fetch_add(&group->rank, 1, __ATOMIC_RELAXED);
    if (state_of(token) != STATE_UNAVAILABLE) {
        group_token_ready(group, token);
    } else {
        // Token sonra hazır olacak: grup o zamana kadar yaşamalı.
        mlirAsyncRuntimeAddRef(group, 1);
        waiters_push(&token->waiters, (Awaiter){NULL, NULL, group, token});
    }
    mr_mutex_unlock(&group->waiters.mu);
    mr_mutex_unlock(&token->waiters.mu);
    return rank;
}

static void set_state(AsyncEvent* event, int8_t state) {
    mr_mutex_lock(&event->waiters.mu);
    if (state_of(event) != STATE_UNAVAILABLE) marlin_rt_panic("async calisma zamani: deger iki kez tamamlandi");
    __atomic_store_n(&event->state, state, __ATOMIC_RELEASE);
    mr_cond_broadcast(&event->waiters.cv);
    waiters_run(&event->waiters);
    mr_mutex_unlock(&event->waiters.mu);
    // Görevin tuttuğu başvuru (2 başvurudan biri) iş bitince bırakılır.
    mlirAsyncRuntimeDropRef(event, 1);
}

void mlirAsyncRuntimeEmplaceToken(void* token) { set_state(token, STATE_AVAILABLE); }
void mlirAsyncRuntimeEmplaceValue(void* value) { set_state(value, STATE_AVAILABLE); }
void mlirAsyncRuntimeSetTokenError(void* token) { set_state(token, STATE_ERROR); }
void mlirAsyncRuntimeSetValueError(void* value) { set_state(value, STATE_ERROR); }

bool mlirAsyncRuntimeIsTokenError(void* token) { return state_of(token) == STATE_ERROR; }
bool mlirAsyncRuntimeIsValueError(void* value) { return state_of(value) == STATE_ERROR; }
bool mlirAsyncRuntimeIsGroupError(void* group) {
    return __atomic_load_n(&((AsyncGroup*)group)->errors, __ATOMIC_ACQUIRE) > 0;
}

static void await_event(AsyncEvent* event) {
    mr_mutex_lock(&event->waiters.mu);
    while (state_of(event) == STATE_UNAVAILABLE) mr_cond_wait(&event->waiters.cv, &event->waiters.mu);
    mr_mutex_unlock(&event->waiters.mu);
}

void mlirAsyncRuntimeAwaitToken(void* token) { await_event(token); }
void mlirAsyncRuntimeAwaitValue(void* value) { await_event(value); }

void mlirAsyncRuntimeAwaitAllInGroup(void* groupObject) {
    AsyncGroup* group = groupObject;
    mr_mutex_lock(&group->waiters.mu);
    while (__atomic_load_n(&group->pending, __ATOMIC_ACQUIRE) != 0)
        mr_cond_wait(&group->waiters.cv, &group->waiters.mu);
    mr_mutex_unlock(&group->waiters.mu);
}

int8_t* mlirAsyncRuntimeGetValueStorage(void* value) { return ((AsyncEvent*)value)->storage; }

void mlirAsyncRuntimeExecute(void* handle, CoroResume resume) { pool_submit(resume, handle); }

// Hazırsa coroutine'i hemen bu iş parçacığında sürdürür, değilse hazır
// olunca (onu hazır yapan iş parçacığında) sürdürülmek üzere kaydeder.
static void await_and_execute(Waiters* w, bool (*ready)(void*), void* object, void* handle, CoroResume resume) {
    mr_mutex_lock(&w->mu);
    if (ready(object)) {
        mr_mutex_unlock(&w->mu);
        resume(handle);
        return;
    }
    waiters_push(w, (Awaiter){resume, handle, NULL, NULL});
    mr_mutex_unlock(&w->mu);
}

static bool event_ready(void* event) { return state_of(event) != STATE_UNAVAILABLE; }
static bool group_ready(void* group) { return __atomic_load_n(&((AsyncGroup*)group)->pending, __ATOMIC_ACQUIRE) == 0; }

void mlirAsyncRuntimeAwaitTokenAndExecute(void* token, void* handle, CoroResume resume) {
    await_and_execute(&((AsyncEvent*)token)->waiters, event_ready, token, handle, resume);
}

void mlirAsyncRuntimeAwaitValueAndExecute(void* value, void* handle, CoroResume resume) {
    await_and_execute(&((AsyncEvent*)value)->waiters, event_ready, value, handle, resume);
}

void mlirAsyncRuntimeAwaitAllInGroupAndExecute(void* group, void* handle, CoroResume resume) {
    await_and_execute(&((AsyncGroup*)group)->waiters, group_ready, group, handle, resume);
}

// (Adındaki yazım hatası MLIR'indir; async-parallel-for bu adla çağırır.)
int64_t mlirAsyncRuntimGetNumWorkerThreads(void) { return worker_count(); }
