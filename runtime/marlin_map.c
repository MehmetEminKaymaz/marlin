// map<K, V>'nin anahtar tablosu (bkz. AST/SemType.h, MAP;
// MLIRBackend/MLIRGeneratorMap.cpp).
//
// map'in anahtarları ve değerleri derlenen kodda sıradan listelerde durur:
// satır r'deki anahtar keys[r], değeri values[r]. Bu dosya yalnızca
// "anahtar → satır" sorusunu cevaplayan tabloyu tutar:
//
//   find(k)         k'nin satırı; yoksa -1
//   insert(k, r)    k'yi r. satır olarak kaydeder (k tabloda yokken)
//   remove(k)       k'yi siler; ondan sonraki satırlar bir öne kayar (listelerde
//                   de eleman silinip sonrakiler kaydırılır, eklenme sırası korunur)
//
// Anahtarlar 64 bitlik değerlerdir: tamsayı, char ve bool genişletilerek,
// string ise string bloğunun adresi olarak gelir (`is_string`). String
// anahtarlar içerikleriyle karşılaştırılır; tablo her string anahtar için
// kendi başvurusunu tutar (marlin_str_retain), silinince ya da tablo
// bırakılınca bırakır.
//
// TABLO
//
// Açık adresleme, doğrusal yoklama. Yuva sayısı ikinin kuvvetidir; dolu ve
// silinmiş (mezar taşı) yuvalar yuva sayısının %70'ini geçince tablo iki katına
// büyür (mezar taşları bu sırada atılır).
//
// İŞ PARÇACIKLARI
//
// Global durum yoktur. Bir map aynı anda yalnızca okunuyorsa (find) birden çok
// iş parçacığından kullanılabilir: paralel döngüde map'e yazmak dilde yasaktır
// ve map göreve verilemez (Sema).

#include "marlin_rt.h"

#include <stdlib.h>
#include <string.h>

void marlin_str_retain(int64_t s);
void marlin_str_release(int64_t s);

enum { SLOT_EMPTY, SLOT_FULL, SLOT_DELETED };

typedef struct {
    uint64_t hash;
    int64_t key;
    int64_t row;
    int32_t state;
} Slot;

typedef struct {
    Slot* slots;
    int64_t capacity;  // ikinin kuvveti
    int64_t count;     // dolu yuvalar
    int64_t used;      // dolu + silinmiş yuvalar
    int32_t is_string; // ilk kullanımda belirlenir; -1: henüz bilinmiyor
} Map;

static Map* map_of(int64_t handle) {
    if (!handle) marlin_rt_panic("map baslatilmamis");
    return (Map*)(intptr_t)handle;
}

static void* checked_calloc(size_t count, size_t size) {
    void* p = calloc(count, size);
    if (!p) marlin_rt_panic("map icin bellek ayrilamadi");
    return p;
}

// --- Anahtarlar ---

// splitmix64'ün son adımı: ardışık tamsayılar da yuvalara iyi dağılır.
static uint64_t hash_int(int64_t key) {
    uint64_t z = (uint64_t)key + 0x9E3779B97F4A7C15ULL;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31);
}

// FNV-1a
static uint64_t hash_string(int64_t s) {
    const unsigned char* data = (const unsigned char*)marlin_rt_str_data(s);
    const int64_t length = marlin_str_len(s);
    uint64_t h = 0xCBF29CE484222325ULL;
    for (int64_t i = 0; i < length; ++i) {
        h ^= data[i];
        h *= 0x100000001B3ULL;
    }
    return h;
}

static uint64_t hash_key(const Map* map, int64_t key) {
    return map->is_string ? hash_string(key) : hash_int(key);
}

static int keys_equal(const Map* map, int64_t a, int64_t b) {
    if (a == b) return 1;
    if (!map->is_string) return 0;
    const int64_t length = marlin_str_len(a);
    return length == marlin_str_len(b) &&
           memcmp(marlin_rt_str_data(a), marlin_rt_str_data(b), (size_t)length) == 0;
}

// Tablonun anahtar türü ilk kullanımda belirlenir; derleyici her map için hep
// aynı türü verir.
static void set_kind(Map* map, int32_t is_string) {
    if (map->is_string < 0) map->is_string = is_string != 0;
}

// --- Tablo ---

// k'nin yuvası; yoksa NULL.
static Slot* lookup(const Map* map, int64_t key, uint64_t hash) {
    const uint64_t mask = (uint64_t)map->capacity - 1;
    for (uint64_t i = hash & mask;; i = (i + 1) & mask) {
        Slot* slot = &map->slots[i];
        if (slot->state == SLOT_EMPTY) return NULL;
        if (slot->state == SLOT_FULL && slot->hash == hash && keys_equal(map, slot->key, key))
            return slot;
    }
}

// Dolu yuvaları yeni bir diziye taşır (mezar taşları atılır).
static void rehash(Map* map, int64_t capacity) {
    Slot* old = map->slots;
    const int64_t oldCapacity = map->capacity;
    map->slots = checked_calloc((size_t)capacity, sizeof(Slot));
    map->capacity = capacity;
    map->used = map->count;
    const uint64_t mask = (uint64_t)capacity - 1;
    for (int64_t i = 0; i < oldCapacity; ++i) {
        if (old[i].state != SLOT_FULL) continue;
        uint64_t j = old[i].hash & mask;
        while (map->slots[j].state != SLOT_EMPTY) j = (j + 1) & mask;
        map->slots[j] = old[i];
    }
    free(old);
}

int64_t marlin_map_new(void) {
    Map* map = checked_calloc(1, sizeof(Map));
    map->capacity = 8;
    map->slots = checked_calloc((size_t)map->capacity, sizeof(Slot));
    map->is_string = -1;
    return (int64_t)(intptr_t)map;
}

int64_t marlin_map_find(int64_t handle, int64_t key, int32_t is_string) {
    Map* map = map_of(handle);
    set_kind(map, is_string);
    Slot* slot = lookup(map, key, hash_key(map, key));
    return slot ? slot->row : -1;
}

void marlin_map_insert(int64_t handle, int64_t key, int32_t is_string, int64_t row) {
    Map* map = map_of(handle);
    set_kind(map, is_string);
    if ((map->used + 1) * 10 > map->capacity * 7) {
        // Yeniden yerleşimden sonra en fazla yarısı dolu olsun. (Yalnızca mezar
        // taşları çoğaldıysa boyut değişmeden temizlenir.)
        int64_t capacity = map->capacity;
        while ((map->count + 1) * 2 > capacity) capacity *= 2;
        rehash(map, capacity);
    }

    const uint64_t hash = hash_key(map, key);
    const uint64_t mask = (uint64_t)map->capacity - 1;
    uint64_t i = hash & mask;
    while (map->slots[i].state == SLOT_FULL) i = (i + 1) & mask;
    if (map->slots[i].state == SLOT_EMPTY) ++map->used;
    map->slots[i] = (Slot){hash, key, row, SLOT_FULL};
    ++map->count;
    if (map->is_string) marlin_str_retain(key);
}

void marlin_map_remove(int64_t handle, int64_t key, int32_t is_string) {
    Map* map = map_of(handle);
    set_kind(map, is_string);
    Slot* slot = lookup(map, key, hash_key(map, key));
    if (!slot) return;
    const int64_t row = slot->row;
    if (map->is_string) marlin_str_release(slot->key);
    slot->state = SLOT_DELETED;
    --map->count;
    // Sonraki satırlar listelerde bir öne kaydı.
    for (int64_t i = 0; i < map->capacity; ++i)
        if (map->slots[i].state == SLOT_FULL && map->slots[i].row > row) --map->slots[i].row;
}

void marlin_map_free(int64_t handle) {
    if (!handle) return;
    Map* map = (Map*)(intptr_t)handle;
    if (map->is_string == 1)
        for (int64_t i = 0; i < map->capacity; ++i)
            if (map->slots[i].state == SLOT_FULL) marlin_str_release(map->slots[i].key);
    free(map->slots);
    free(map);
}

// m[k]: anahtar yok. `kind`: anahtarın marlin tipi, mesajda nasıl yazılacağı
// için (0 işaretli tamsayı, 1 işaretsiz tamsayı, 2 char, 3 bool, 4 string).
void marlin_panic_map_key(int64_t key, int32_t kind, int32_t line) {
    char text[64];
    switch (kind) {
        case 1: snprintf(text, sizeof text, "%llu", (unsigned long long)key); break;
        case 2: snprintf(text, sizeof text, "'%c'", (char)key); break;
        case 3: snprintf(text, sizeof text, "%s", key ? "true" : "false"); break;
        case 4:
            marlin_rt_panic("map'te \"%s\" anahtari yok (satir %d; once has ile bakin)",
                            marlin_rt_str_data(key), (int)line);
            return;
        default: snprintf(text, sizeof text, "%lld", (long long)key); break;
    }
    marlin_rt_panic("map'te %s anahtari yok (satir %d; once has ile bakin)", text, (int)line);
}
