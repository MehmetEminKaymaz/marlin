// JSON ayrıştırıcısı (RFC 8259).
//
// Yalnızca ayrıştırır: G/Ç yapmaz, global durum tutmaz; aynı anda birçok iş
// parçacığından kullanılabilir. Sonuç bir belgedir (JsonDoc): bütün düğümler
// onun dizilerinde durur ve belgeyle birlikte tek seferde bırakılır
// (json_free). Düğümlere sıra numarasıyla (int64) erişilir; kök düğüm 0'dır.
//
// KURALLAR (katı)
//
//   * Kök her değer olabilir (yalnızca nesne / dizi değil).
//   * Boşluk: boşluk, sekme, \n, \r. Değerden sonra başka bir şey olamaz.
//   * Sayı: -?(0|[1-9][0-9]*)(.[0-9]+)?([eE][+-]?[0-9]+)?  — baştaki +, 0x,
//     .5, NaN, Infinity yok. Kesirsiz ve üssüz olup int64'e sığan sayı
//     tamsayıdır; diğerleri float64. float64'e sığmayan (1e999) hatadır.
//   * String: UTF-8 doğrulanır; denetim karakterleri (< 0x20) kaçışsız
//     yazılamaz; \uXXXX kaçışları vekil çiftleriyle (surrogate pair) UTF-8'e
//     çevrilir, eşi olmayan vekil hatadır.
//   * Nesnede aynı anahtar birden çok kez geçerse sonuncusu geçerlidir.
//   * İç içelik en fazla JSON_MAX_DEPTH: kötü niyetli girdi ([[[[...)
//     yığıtı taşıramaz.
//
// Hata mesajı ASCII'dir ve konum içerir: "satir 3, sutun 12: ..." (sütun bayt sayar).

#ifndef MARLIN_JSON_PARSER_H
#define MARLIN_JSON_PARSER_H

#include <stddef.h>
#include <stdint.h>

#define JSON_MAX_DEPTH 256

typedef enum {
    JSON_MISSING = 0,  // olmayan düğüm (ör. nesnede olmayan anahtar); düğüm numarası -1
    JSON_NULL,
    JSON_BOOL,
    JSON_NUMBER,
    JSON_STRING,
    JSON_ARRAY,
    JSON_OBJECT,
} JsonKind;

typedef struct {
    int32_t kind;
    int32_t is_integer;   // sayı: tamsayı mı?
    int64_t integer;      // sayı (tamsayıysa) / bool (0, 1)
    double number;        // sayı (her zaman; tamsayıda da)
    int64_t offset;       // string: pool içindeki yeri; dizi / nesne: children içindeki yeri
    int64_t length;       // string: bayt sayısı; dizi / nesne: eleman sayısı
    int64_t key;          // nesne üyesiyse anahtarın pool içindeki yeri; değilse -1
    int64_t key_length;
} JsonNode;

typedef struct {
    JsonNode* nodes;
    int64_t node_count, node_capacity;
    int64_t* children;   // dizi / nesne elemanlarının düğüm numaraları, ardışık
    int64_t child_count, child_capacity;
    char* pool;          // çözülmüş stringler ve anahtarlar
    int64_t pool_length, pool_capacity;
    int ok;
    char error[256];
} JsonDoc;

// Her zaman bir belge döner (bellek yetmezse NULL). Geçersiz girdide ok = 0
// ve error doludur; düğüm yoktur.
JsonDoc* json_parse(const char* text, size_t length);
void json_free(JsonDoc* doc);

// --- Okuma (düğüm numarası -1 ya da sınır dışıysa JSON_MISSING gibi davranır) ---
JsonKind json_kind(const JsonDoc* doc, int64_t node);
// Nesnenin `key` anahtarlı üyesi; yoksa -1.
int64_t json_member(const JsonDoc* doc, int64_t object, const char* key, size_t key_length);
// Dizinin / nesnenin i. elemanı; yoksa -1.
int64_t json_child(const JsonDoc* doc, int64_t node, int64_t index);

// Değeri JSON string literali olarak yazar ("..." ve kaçışlarla) `out`a;
// gereken bayt sayısını döndürür (out NULL olabilir: yalnızca ölçer).
size_t json_quote(const char* text, size_t length, char* out);

#endif // MARLIN_JSON_PARSER_H
