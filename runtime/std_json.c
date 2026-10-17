// std.json — JSON belgelerinin C tarafı (bkz. std/json.marl, json_parser.h).
//
// Belge marlin tarafına adres olarak (int64) verilir; düğümler belgedeki sıra
// numaralarıdır (kök 0, olmayan düğüm -1). Belge değişmez: aynı anda birçok
// iş parçacığından okunabilir. Global durum yoktur.
//
// Güvenilmeyen girdi programı durdurmaz: ayrıştırma hatası belgenin ok / error
// alanlarındadır; yanlış tipte okuma (as_int'e string gibi) sıfır değeri verir.
// Programı yalnızca kullanım hatası durdurur (bırakılmış belge).

#include "marlin_rt.h"
#include "json_parser.h"

#include <stdlib.h>

static JsonDoc* doc_of(int64_t handle) {
    if (!handle) marlin_rt_panic("json: belge gecersiz (free ile birakilmis olabilir)");
    return (JsonDoc*)(intptr_t)handle;
}

static int64_t pool_string(const JsonDoc* doc, int64_t offset, int64_t length) {
    if (length == 0) return marlin_rt_str_from_text("");
    return marlin_rt_str_new(doc->pool + offset, length);
}

static const JsonNode* node_of(const JsonDoc* doc, int64_t node) {
    return json_kind(doc, node) == JSON_MISSING ? NULL : &doc->nodes[node];
}

int64_t marlin_json_parse(int64_t text) {
    JsonDoc* doc = json_parse(marlin_rt_str_data(text), (size_t)marlin_str_len(text));
    if (!doc) marlin_rt_panic("json: bellek yetersiz");
    return (int64_t)(intptr_t)doc;
}

int32_t marlin_json_ok(int64_t d) { return doc_of(d)->ok; }
int64_t marlin_json_error(int64_t d) { return marlin_rt_str_from_text(doc_of(d)->error); }

void marlin_json_free(int64_t d) {
    if (d) json_free((JsonDoc*)(intptr_t)d);
}

// 0 yok, 1 null, 2 bool, 3 sayı, 4 string, 5 dizi, 6 nesne (JsonKind)
int32_t marlin_json_kind(int64_t d, int64_t node) { return (int32_t)json_kind(doc_of(d), node); }

int64_t marlin_json_get(int64_t d, int64_t node, int64_t key) {
    return json_member(doc_of(d), node, marlin_rt_str_data(key), (size_t)marlin_str_len(key));
}

int64_t marlin_json_at(int64_t d, int64_t node, int64_t index) { return json_child(doc_of(d), node, index); }

// Dizi / nesnenin eleman sayısı; diğerlerinde 0.
int64_t marlin_json_length(int64_t d, int64_t node) {
    const JsonDoc* doc = doc_of(d);
    const JsonKind kind = json_kind(doc, node);
    return kind == JSON_ARRAY || kind == JSON_OBJECT ? doc->nodes[node].length : 0;
}

// Nesnenin i. üyesinin anahtarı; yoksa "".
int64_t marlin_json_key_at(int64_t d, int64_t node, int64_t index) {
    const JsonDoc* doc = doc_of(d);
    if (json_kind(doc, node) != JSON_OBJECT) return marlin_rt_str_from_text("");
    const JsonNode* member = node_of(doc, json_child(doc, node, index));
    return member ? pool_string(doc, member->key, member->key_length) : marlin_rt_str_from_text("");
}

int64_t marlin_json_text(int64_t d, int64_t node) {
    const JsonDoc* doc = doc_of(d);
    const JsonNode* n = node_of(doc, node);
    if (!n || n->kind != JSON_STRING) return marlin_rt_str_from_text("");
    return pool_string(doc, n->offset, n->length);
}

int32_t marlin_json_is_integer(int64_t d, int64_t node) {
    const JsonNode* n = node_of(doc_of(d), node);
    return n && n->kind == JSON_NUMBER && n->is_integer;
}

// Tamsayı; kesirli sayı sıfıra doğru kesilir (int64'e sığmıyorsa 0). Sayı
// değilse 0.
int64_t marlin_json_integer(int64_t d, int64_t node) {
    const JsonNode* n = node_of(doc_of(d), node);
    if (!n || n->kind != JSON_NUMBER) return 0;
    if (n->is_integer) return n->integer;
    if (n->number >= -9223372036854775808.0 && n->number < 9223372036854775808.0) return (int64_t)n->number;
    return 0;
}

double marlin_json_number(int64_t d, int64_t node) {
    const JsonNode* n = node_of(doc_of(d), node);
    return n && n->kind == JSON_NUMBER ? n->number : 0.0;
}

int32_t marlin_json_bool(int64_t d, int64_t node) {
    const JsonNode* n = node_of(doc_of(d), node);
    return n && n->kind == JSON_BOOL && n->integer;
}

// "..." — JSON string literali olarak (kaçışlarla).
int64_t marlin_json_quote(int64_t text) {
    const char* s = marlin_rt_str_data(text);
    const size_t length = (size_t)marlin_str_len(text);
    int64_t result;
    char* out = marlin_rt_str_alloc((int64_t)json_quote(s, length, NULL), &result);
    json_quote(s, length, out);
    return result;
}
