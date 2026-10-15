// std.random — rastgele sayılar (bkz. std/random.marl).
//
// Üreteç (splitmix64) kütüphanenin kendisindedir: aynı tohum her makinede aynı
// diziyi verir. Tohum verilmezse program her çalıştığında aynı sayılar gelir
// (ölçümlerin tekrarlanabilir olması için). Üretecin durumu tektir ve
// kilitsizdir; bu yüzden fonksiyonlar yan etkili sayılır (paralel döngüden
// çağrılamaz).

#include "marlin_rt.h"

static uint64_t state = 0x853C49E6748FEA9BULL;

static uint64_t next(void) {
    uint64_t z = (state += 0x9E3779B97F4A7C15ULL);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31);
}

void marlin_random_seed(int64_t seed) { state = (uint64_t)seed; }

// [0, 1) aralığında.
double marlin_random_random(void) { return (double)(next() >> 11) * (1.0 / 9007199254740992.0); }

// [low, high) aralığında bir tamsayı.
int64_t marlin_random_rand_int(int64_t low, int64_t high) {
    if (high <= low)
        marlin_rt_panic("rand_int: aralik bos (%lld .. %lld)", (long long)low, (long long)high);
    return low + (int64_t)(next() % (uint64_t)(high - low));
}
