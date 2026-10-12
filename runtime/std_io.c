// std.io — klavyeden okuma ve satır sonu eklemeden yazma (bkz. std/io.minik).

#define _GNU_SOURCE // flockfile

#include "minik_rt.h"

// Okumadan önce bekleyen çıktı yazılır: kullanıcı, cevap vereceği soruyu görmeli.
int64_t minik_io_read_line(void) {
    fflush(stdout);
    return minik_rt_read_line(stdin);
}

int32_t minik_io_eof(void) {
    fflush(stdout);
    return minik_rt_at_eof(stdin);
}

// print'ten farkı: satır sonu eklemez.
void minik_io_write(int64_t text) {
    flockfile(stdout);
    fwrite(minik_rt_str_data(text), 1, (size_t)minik_str_len(text), stdout);
    funlockfile(stdout);
}
