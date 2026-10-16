// Görevler (task<T>) — `marl f(x)` ile başlatılan görevin handle'ı.
//
// Görevin kendisi MLIR'in async çalışma zamanında yaşar (libmlir_async_runtime:
// iş parçacığı havuzu, görevlerin bitişini bildiren nesneler). Derleyici
// `marl f(x)` için bir async.execute üretir; onun sonucu bir AsyncValue'dur
// (sonucu olmayan görevde AsyncToken). marlin tarafında ise görev, string
// gibi başvuru sayılan bir handle'dır (bkz. MLIRBackend/MLIRGeneratorTask.cpp):
//
//   handle ──▶ [ refs | async ──▶ AsyncValue (sonuç burada) | kind ]
//
// Handle, AsyncValue'nun bir başvurusunu tutar. Handle'ı tutan son yer onu
// bırakınca o başvuru da bırakılır; görev hâlâ çalışıyorsa bundan etkilenmez
// (çalışma zamanı kendi başvurusunu görev bitene kadar tutar).
//
// Sonucu string olan görevde sonuç, AsyncValue'nun içinde bir başvuru olarak
// durur. Handle bırakılırken, görev bittiğinde (ya da bitmişse hemen) o
// string de bırakılır.
//
// Bu dosyada global durum yoktur; sayaçlar atomiktir.

#include "marlin_rt.h"

#include <stdlib.h>
#include <string.h>

// MLIR async çalışma zamanının C arayüzü (mlir/ExecutionEngine/AsyncRuntime.h).
extern void mlirAsyncRuntimeAddRef(void* object, int64_t count);
extern void mlirAsyncRuntimeDropRef(void* object, int64_t count);
extern int8_t* mlirAsyncRuntimeGetValueStorage(void* value);
extern void mlirAsyncRuntimeAwaitValueAndExecute(void* value, void* handle, void (*resume)(void*));

void marlin_str_release(int64_t s);

enum TaskKind {
    TASK_VOID,    // sonucu yok: async = AsyncToken
    TASK_PLAIN,   // sonuç sahiplik gerektirmeyen bir değer (sayı, fonksiyon, liste)
    TASK_STRING,  // sonuç bir string başvurusu
};

typedef struct {
    int64_t refs;
    void* async;
    int32_t kind;
} MarlinTask;

static MarlinTask* task_of(int64_t handle, const char* operation) {
    if (!handle) marlin_rt_panic("%s: baslatilmamis bir gorev", operation);
    return (MarlinTask*)(intptr_t)handle;
}

static int64_t task_new(void* async, int32_t kind) {
    MarlinTask* task = malloc(sizeof *task);
    if (!task) marlin_rt_panic("gorev icin bellek ayrilamadi");
    task->refs = 1;
    task->async = async;
    task->kind = kind;
    return (int64_t)(intptr_t)task;
}

// Derleyicinin ürettiği kod, async.execute'un sonucunu buraya verir; bir
// başvurusu (+1) handle'a geçer. `isString`: sonuç bir string mi?
int64_t marlin_task_wrap(void* value, int32_t isString) {
    return task_new(value, isString ? TASK_STRING : TASK_PLAIN);
}

int64_t marlin_task_wrap_token(void* token) { return task_new(token, TASK_VOID); }

// await için: görevin AsyncValue'su (AsyncToken'ı). Çağıran yeni bir başvuru
// (+1) alır; derleyicinin ürettiği kod onu beklemeden sonra bırakır.
void* marlin_task_value(int64_t handle) {
    MarlinTask* task = task_of(handle, "await");
    mlirAsyncRuntimeAddRef(task->async, 1);
    return task->async;
}

void* marlin_task_token(int64_t handle) { return marlin_task_value(handle); }

void marlin_task_retain(int64_t handle) {
    if (!handle) return;
    __atomic_add_fetch(&((MarlinTask*)(intptr_t)handle)->refs, 1, __ATOMIC_RELAXED);
}

// Görev bitti (ya da zaten bitmişti): sonucu olan stringi ve AsyncValue'yu bırak.
static void release_string_result(void* argument) {
    MarlinTask* task = argument;
    int64_t result;
    memcpy(&result, mlirAsyncRuntimeGetValueStorage(task->async), sizeof result);
    marlin_str_release(result);
    mlirAsyncRuntimeDropRef(task->async, 1);
    free(task);
}

void marlin_task_release(int64_t handle) {
    if (!handle) return;
    MarlinTask* task = (MarlinTask*)(intptr_t)handle;
    if (__atomic_sub_fetch(&task->refs, 1, __ATOMIC_ACQ_REL) != 0) return;
    if (task->kind == TASK_STRING) {
        // Sonuç ancak görev bitince yazılır; bitmediyse bitince bırakılır.
        mlirAsyncRuntimeAwaitValueAndExecute(task->async, task, release_string_result);
        return;
    }
    mlirAsyncRuntimeDropRef(task->async, 1);
    free(task);
}
