// std.io — klavyeden okuma ve satır sonu eklemeden yazma (bkz. std/io.marl).

#ifndef _GNU_SOURCE
#define _GNU_SOURCE // flockfile
#endif

#include "marlin_rt.h"

// Okumadan önce bekleyen çıktı yazılır: kullanıcı, cevap vereceği soruyu görmeli.
int64_t marlin_io_read_line(void) {
    fflush(stdout);
    return marlin_rt_read_line(stdin);
}

int32_t marlin_io_eof(void) {
    fflush(stdout);
    return marlin_rt_at_eof(stdin);
}

// print'ten farkı: satır sonu eklemez.
void marlin_io_write(int64_t text) {
    flockfile(stdout);
    fwrite(marlin_rt_str_data(text), 1, (size_t)marlin_str_len(text), stdout);
    funlockfile(stdout);
}
