// std.string — string fonksiyonlarının C tarafı (bkz. std/string.minik).
//
// Hepsi yan etkisizdir (@pure): yalnızca argümanlarını okur ve sonucu yeni
// bir string olarak döndürür. Uzunluklar ve konumlar bayt sayar.

#include "minik_rt.h"

#include <string.h>

// s'in `start` konumundan başlayan `length` baytı.
int64_t minik_string_substr(int64_t s, int64_t start, int64_t length) {
    const int64_t size = minik_str_len(s);
    if (start < 0 || length < 0 || start + length > size)
        minik_rt_panic("substr: aralik stringin disinda (baslangic %lld, uzunluk %lld, "
                       "string uzunlugu %lld)",
                       (long long)start, (long long)length, (long long)size);
    return minik_rt_str_new(minik_rt_str_data(s) + start, length);
}

// part'ın s içinde, `start` konumundan itibaren ilk geçtiği yer; yoksa -1.
int64_t minik_string_find(int64_t s, int64_t part, int64_t start) {
    const int64_t size = minik_str_len(s), partSize = minik_str_len(part);
    const char* data = minik_rt_str_data(s);
    const char* wanted = minik_rt_str_data(part);
    if (start < 0) start = 0;
    for (int64_t i = start; i + partSize <= size; ++i)
        if (memcmp(data + i, wanted, (size_t)partSize) == 0) return i;
    return -1;
}

int32_t minik_string_starts_with(int64_t s, int64_t prefix) {
    const int64_t size = minik_str_len(s), prefixSize = minik_str_len(prefix);
    return prefixSize <= size &&
           memcmp(minik_rt_str_data(s), minik_rt_str_data(prefix), (size_t)prefixSize) == 0;
}

int32_t minik_string_ends_with(int64_t s, int64_t suffix) {
    const int64_t size = minik_str_len(s), suffixSize = minik_str_len(suffix);
    return suffixSize <= size &&
           memcmp(minik_rt_str_data(s) + size - suffixSize, minik_rt_str_data(suffix),
                  (size_t)suffixSize) == 0;
}

// s içindeki her `from` yerine `to` konmuş hali. `from` boşsa s'in kopyası.
int64_t minik_string_replace(int64_t s, int64_t from, int64_t to) {
    const int64_t size = minik_str_len(s), fromSize = minik_str_len(from), toSize = minik_str_len(to);
    const char* data = minik_rt_str_data(s);
    if (fromSize == 0) return minik_rt_str_new(data, size);

    int64_t count = 0;
    for (int64_t at = minik_string_find(s, from, 0); at >= 0;
         at = minik_string_find(s, from, at + fromSize))
        ++count;

    int64_t result;
    char* out = minik_rt_str_alloc(size + count * (toSize - fromSize), &result);
    int64_t position = 0;
    for (int64_t at = minik_string_find(s, from, 0); at >= 0;
         at = minik_string_find(s, from, position)) {
        memcpy(out, data + position, (size_t)(at - position));
        out += at - position;
        memcpy(out, minik_rt_str_data(to), (size_t)toSize);
        out += toSize;
        position = at + fromSize;
    }
    memcpy(out, data + position, (size_t)(size - position));
    return result;
}

static int is_blank(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; }

// Baştaki ve sondaki boşluklar (boşluk, sekme, satır sonu) atılmış hali.
int64_t minik_string_trim(int64_t s) {
    const char* data = minik_rt_str_data(s);
    int64_t begin = 0, end = minik_str_len(s);
    while (begin < end && is_blank(data[begin])) ++begin;
    while (end > begin && is_blank(data[end - 1])) --end;
    return minik_rt_str_new(data + begin, end - begin);
}

// Yalnızca İngilizce harfler (a-z, A-Z) çevrilir: metin UTF-8'dir ve Türkçe
// harfler birden çok bayttan oluşur; onlara dokunulmaz.
static int64_t map_case(int64_t s, char first, char last, int shift) {
    const int64_t size = minik_str_len(s);
    const char* data = minik_rt_str_data(s);
    int64_t result;
    char* out = minik_rt_str_alloc(size, &result);
    for (int64_t i = 0; i < size; ++i)
        out[i] = (data[i] >= first && data[i] <= last) ? (char)(data[i] + shift) : data[i];
    return result;
}

int64_t minik_string_upper(int64_t s) { return map_case(s, 'a', 'z', 'A' - 'a'); }

int64_t minik_string_lower(int64_t s) { return map_case(s, 'A', 'Z', 'a' - 'A'); }
