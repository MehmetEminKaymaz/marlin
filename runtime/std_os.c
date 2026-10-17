// std.os — komut satırı argümanları, ortam değişkenleri, çıkış (bkz. std/os.marl).

#include "marlin_rt.h"
#include "platform.h"

#include <stdlib.h>

// Programın `main`i parametresizdir; argümanları platform katmanı program
// başlamadan önce kaydeder (bkz. platform_posix.c, platform_win32.c).

int64_t marlin_os_arg_count(void) { return mr_arg_count(); }

// 0: programın adı, 1..: argümanlar.
int64_t marlin_os_arg(int64_t index) {
    if (index < 0 || index >= mr_arg_count())
        marlin_rt_panic("arg: indeks sinir disinda (indeks %lld, arguman sayisi %d)",
                       (long long)index, mr_arg_count());
    return marlin_rt_str_from_text(mr_arg((int)index));
}

// Ortam değişkeninin değeri; tanımlı değilse boş string.
int64_t marlin_os_env(int64_t name) {
    char* value = mr_getenv(marlin_rt_str_data(name));
    const int64_t result = marlin_rt_str_from_text(value ? value : "");
    free(value);
    return result;
}

// Programı hemen bitirir. (_Exit: bkz. marlin_rt.c, fail.)
void marlin_os_exit(int32_t code) {
    fflush(stdout);
    fflush(stderr);
    _Exit(code);
}
