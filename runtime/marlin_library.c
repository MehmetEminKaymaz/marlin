// Dış C kütüphanesini program çalışırken yükleme (bkz. marlin_rt.h).
//
// GPU sürücülerinde olduğu gibi (marlin_gpu.c) kütüphane ancak gerekince
// açılır ve fonksiyonları adlarıyla bulunur. Program derlenirken yalnızca
// kütüphanenin başlıkları (tipler, sabitler) gerekir.

#include "marlin_rt.h"
#include "platform.h"

#include <stdlib.h>
#include <string.h>

static int load(MarlinLibrary* lib, const char* what, const char* env, const char* const* candidates,
                const MarlinSymbol* symbols, const char* hint) {
    void* handle = NULL;
    char* path = env ? mr_getenv(env) : NULL;
    if (path && path[0]) {
        handle = mr_library_open(path);
        if (!handle) {
            snprintf(lib->error, sizeof lib->error, "%s acilamadi: %s (%s ortam degiskeni)", what, path, env);
            free(path);
            return 0;
        }
    } else {
        for (const char* const* name = candidates; *name && !handle; ++name) handle = mr_library_open(*name);
        if (!handle) {
            snprintf(lib->error, sizeof lib->error, "%s bulunamadi (%s%s%s%s)", what, hint,
                     env ? "; yolu " : "", env ? env : "", env ? " ortam degiskeniyle verilebilir" : "");
            free(path);
            return 0;
        }
    }
    free(path);
    for (const MarlinSymbol* symbol = symbols; symbol->name; ++symbol) {
        void* address = mr_library_symbol(handle, symbol->name);
        if (!address) {
            snprintf(lib->error, sizeof lib->error, "%s cok eski ya da bozuk: '%s' fonksiyonu yok", what, symbol->name);
            return 0;
        }
        // (Fonksiyon işaretçisine nesne işaretçisinden kopyalama: dlsym'in
        // bilinen kullanımı; desteklenen bütün platformlarda boyutları aynı.)
        memcpy(symbol->slot, &address, sizeof address);
    }
    return 1;
}

int marlin_rt_load_library(MarlinLibrary* lib, const char* what, const char* env,
                           const char* const* candidates, const MarlinSymbol* symbols, const char* hint) {
    int state = __atomic_load_n(&lib->state, __ATOMIC_ACQUIRE);
    if (state < 2) {
        int expected = 0;
        if (__atomic_compare_exchange_n(&lib->state, &expected, 1, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
            state = load(lib, what, env, candidates, symbols, hint) ? 2 : 3;
            __atomic_store_n(&lib->state, state, __ATOMIC_RELEASE);
        } else {
            while ((state = __atomic_load_n(&lib->state, __ATOMIC_ACQUIRE)) < 2) mr_sleep_seconds(0.001);
        }
    }
    return state == 2;
}
