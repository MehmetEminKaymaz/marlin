// std.os — komut satırı argümanları, ortam değişkenleri, çıkış (bkz. std/os.minik).

#include "minik_rt.h"

#include <stdlib.h>
#include <unistd.h>

// Programın `main`i parametresizdir; argümanlar program başlamadan önce
// buraya kaydedilir. (Linux'ta C kütüphanesi, başlangıç fonksiyonlarına
// main'in argümanlarını verir.)
static int saved_argc;
static char** saved_argv;

__attribute__((constructor))
static void save_arguments(int argc, char** argv) {
    saved_argc = argc;
    saved_argv = argv;
}

int64_t minik_os_arg_count(void) { return saved_argc; }

// 0: programın adı, 1..: argümanlar.
int64_t minik_os_arg(int64_t index) {
    if (index < 0 || index >= saved_argc)
        minik_rt_panic("arg: indeks sinir disinda (indeks %lld, arguman sayisi %d)",
                       (long long)index, saved_argc);
    return minik_rt_str_from_text(saved_argv[index]);
}

// Ortam değişkeninin değeri; tanımlı değilse boş string.
int64_t minik_os_env(int64_t name) {
    const char* value = getenv(minik_rt_str_data(name));
    return minik_rt_str_from_text(value ? value : "");
}

// Programı hemen bitirir. (_exit: bkz. minik_rt.c, fail.)
void minik_os_exit(int32_t code) {
    fflush(stdout);
    fflush(stderr);
    _exit(code);
}
