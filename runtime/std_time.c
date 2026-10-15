// std.time — takvim zamanı ve bekleme (bkz. std/time.marl).

#define _POSIX_C_SOURCE 200809L // clock_gettime, nanosleep

#include "marlin_rt.h"

#include <time.h>

// 1 Ocak 1970'ten beri geçen saniye (kesirli). Süre ölçmek için yerleşik
// time() daha uygundur: o saat geri gitmez.
double marlin_time_now(void) {
    struct timespec now;
    clock_gettime(CLOCK_REALTIME, &now);
    return (double)now.tv_sec + (double)now.tv_nsec / 1e9;
}

void marlin_time_sleep(double seconds) {
    if (seconds <= 0) return;
    struct timespec duration;
    duration.tv_sec = (time_t)seconds;
    duration.tv_nsec = (long)((seconds - (double)duration.tv_sec) * 1e9);
    nanosleep(&duration, NULL);
}
