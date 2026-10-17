// std.json — JSON belgelerinin C tarafı (bkz. std/json.marl, json_parser.h).
//
// Belge marlin tarafına adres olarak (int64) verilir; düğümler belgedeki sıra
// numaralarıdır (kök 0, olmayan düğüm -1). Global durum yoktur.
//
// Güvenilmeyen girdi programı durdurmaz: ayrıştırma hatası belgenin ok / error
// alanlarındadır; yanlış tipte okuma (as_int'e string gibi) sıfır değeri verir.
// Programı yalnızca kullanım hatası durdurur (bırakılmış belge).
//
// from_json / to_json (derleyicinin yerleşikleri; bkz.
// MLIRBackend/MLIRGeneratorJson.cpp) buradaki marlin_json_decode_* ve
// marlin_json_writer_* fonksiyonlarını çağırır.
//
// İŞ PARÇACIKLARI
//
// Ayrıştırılmış belge değişmezdir ve aynı anda okunabilir. Tek istisna
// from_json'un kaydettiği hata: aynı belge aynı anda iki görevde from_json ile
// çözülmemelidir (her görev kendi belgesini ayrıştırmalıdır).

#include "marlin_rt.h"
#include "json_parser.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

int64_t marlin_str_from_f64(double value);
int64_t marlin_str_from_f32(float value);
void marlin_str_release(int64_t s);

typedef struct {
    JsonDoc* json;
    int decode_failed;      // son from_json'da hata oldu mu? (ilki kaydedilir)
    char decode_error[320];
} Document;

static Document* doc_of(int64_t handle) {
    if (!handle) marlin_rt_panic("json: belge gecersiz (free ile birakilmis olabilir)");
    return (Document*)(intptr_t)handle;
}

static int64_t pool_string(const JsonDoc* doc, int64_t offset, int64_t length) {
    if (length == 0) return marlin_rt_str_from_text("");
    return marlin_rt_str_new(doc->pool + offset, length);
}

static const JsonNode* node_of(const JsonDoc* doc, int64_t node) {
    return json_kind(doc, node) == JSON_MISSING ? NULL : &doc->nodes[node];
}

// ===========================================================================
// Belge ve okuma (std/json.marl)
// ===========================================================================

int64_t marlin_json_parse(int64_t text) {
    Document* d = calloc(1, sizeof *d);
    if (d) d->json = json_parse(marlin_rt_str_data(text), (size_t)marlin_str_len(text));
    if (!d || !d->json) marlin_rt_panic("json: bellek yetersiz");
    return (int64_t)(intptr_t)d;
}

int32_t marlin_json_ok(int64_t d) { return doc_of(d)->json->ok; }
int64_t marlin_json_error(int64_t d) { return marlin_rt_str_from_text(doc_of(d)->json->error); }

void marlin_json_free(int64_t handle) {
    if (!handle) return;
    Document* d = (Document*)(intptr_t)handle;
    json_free(d->json);
    free(d);
}

// 0 yok, 1 null, 2 bool, 3 sayı, 4 string, 5 dizi, 6 nesne (JsonKind)
int32_t marlin_json_kind(int64_t d, int64_t node) { return (int32_t)json_kind(doc_of(d)->json, node); }

int64_t marlin_json_get(int64_t d, int64_t node, int64_t key) {
    return json_member(doc_of(d)->json, node, marlin_rt_str_data(key), (size_t)marlin_str_len(key));
}

int64_t marlin_json_at(int64_t d, int64_t node, int64_t index) {
    return json_child(doc_of(d)->json, node, index);
}

// Dizi / nesnenin eleman sayısı; diğerlerinde 0.
int64_t marlin_json_length(int64_t d, int64_t node) {
    const JsonDoc* doc = doc_of(d)->json;
    const JsonKind kind = json_kind(doc, node);
    return kind == JSON_ARRAY || kind == JSON_OBJECT ? doc->nodes[node].length : 0;
}

// Nesnenin i. üyesinin anahtarı; yoksa "".
int64_t marlin_json_key_at(int64_t d, int64_t node, int64_t index) {
    const JsonDoc* doc = doc_of(d)->json;
    if (json_kind(doc, node) != JSON_OBJECT) return marlin_rt_str_from_text("");
    const JsonNode* member = node_of(doc, json_child(doc, node, index));
    return member ? pool_string(doc, member->key, member->key_length) : marlin_rt_str_from_text("");
}

int64_t marlin_json_text(int64_t d, int64_t node) {
    const JsonDoc* doc = doc_of(d)->json;
    const JsonNode* n = node_of(doc, node);
    if (!n || n->kind != JSON_STRING) return marlin_rt_str_from_text("");
    return pool_string(doc, n->offset, n->length);
}

int32_t marlin_json_is_integer(int64_t d, int64_t node) {
    const JsonNode* n = node_of(doc_of(d)->json, node);
    return n && n->kind == JSON_NUMBER && n->is_integer;
}

// Tamsayı; kesirli sayı sıfıra doğru kesilir (int64'e sığmıyorsa 0). Sayı
// değilse 0.
int64_t marlin_json_integer(int64_t d, int64_t node) {
    const JsonNode* n = node_of(doc_of(d)->json, node);
    if (!n || n->kind != JSON_NUMBER) return 0;
    if (n->is_integer) return n->integer;
    if (n->number >= -9223372036854775808.0 && n->number < 9223372036854775808.0) return (int64_t)n->number;
    return 0;
}

double marlin_json_number(int64_t d, int64_t node) {
    const JsonNode* n = node_of(doc_of(d)->json, node);
    return n && n->kind == JSON_NUMBER ? n->number : 0.0;
}

int32_t marlin_json_bool(int64_t d, int64_t node) {
    const JsonNode* n = node_of(doc_of(d)->json, node);
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

// ===========================================================================
// from_json: tipli okuma
//
// Olmayan değer ve null, alanın sıfır değeri olur (hata değildir). Yanlış
// tipte değer de sıfır değer olur, ama hata kaydedilir: belgenin
// decode_error'ı ilk hatayı, değerin yolu ile birlikte söyler.
// ===========================================================================

static const char* kind_name(JsonKind kind) {
    switch (kind) {
        case JSON_NULL: return "null";
        case JSON_BOOL: return "bool";
        case JSON_NUMBER: return "sayi";
        case JSON_STRING: return "string";
        case JSON_ARRAY: return "dizi";
        case JSON_OBJECT: return "nesne";
        default: return "yok";
    }
}

// Kökten `target`a giden yol: $.kullanicilar[2].ad. Bulunduysa 1.
static int find_path(const JsonDoc* doc, int64_t node, int64_t target, char* out, size_t size, size_t used) {
    if (node == target) return 1;
    const JsonNode* n = &doc->nodes[node];
    if (n->kind != JSON_ARRAY && n->kind != JSON_OBJECT) return 0;
    for (int64_t i = 0; i < n->length; ++i) {
        const int64_t child = doc->children[n->offset + i];
        const JsonNode* c = &doc->nodes[child];
        int written;
        if (n->kind == JSON_ARRAY)
            written = snprintf(out + used, size - used, "[%lld]", (long long)i);
        else
            written = snprintf(out + used, size - used, ".%.*s", (int)c->key_length, doc->pool + c->key);
        if (written < 0) return 0;
        const size_t next = used + (size_t)written < size ? used + (size_t)written : size - 1;
        if (find_path(doc, child, target, out, size, next)) return 1;
        out[used] = '\0';
    }
    return 0;
}

static void decode_fail(Document* d, int64_t node, const char* expected, const char* found) {
    if (d->decode_failed) return;
    d->decode_failed = 1;
    char path[200] = "$";
    if (node > 0) find_path(d->json, 0, node, path, sizeof path, 1);
    snprintf(d->decode_error, sizeof d->decode_error, "%s: %s bekleniyordu, %s bulundu", path, expected, found);
}

// Her from_json başında: önceki hata silinir.
void marlin_json_decode_begin(int64_t d) {
    Document* doc = doc_of(d);
    doc->decode_failed = 0;
    doc->decode_error[0] = '\0';
}

// Son from_json'un hatası; hata yoksa "".
int64_t marlin_json_decode_error(int64_t d) { return marlin_rt_str_from_text(doc_of(d)->decode_error); }

// Düğüm beklenen türde mi? Olmayan / null düğüm hata değildir (0 döner).
int32_t marlin_json_decode_expect(int64_t d, int64_t node, int32_t kind) {
    Document* doc = doc_of(d);
    const JsonKind actual = json_kind(doc->json, node);
    if (actual == (JsonKind)kind) return 1;
    if (actual != JSON_MISSING && actual != JSON_NULL)
        decode_fail(doc, node, kind_name((JsonKind)kind), kind_name(actual));
    return 0;
}

// `bits` bitlik, `is_signed` işaretli tamsayı.
int64_t marlin_json_decode_int(int64_t d, int64_t node, int32_t bits, int32_t is_signed) {
    Document* doc = doc_of(d);
    if (!marlin_json_decode_expect(d, node, JSON_NUMBER)) return 0;
    const JsonNode* n = &doc->json->nodes[node];
    char expected[48];
    snprintf(expected, sizeof expected, "%s%d tamsayi", is_signed ? "int" : "uint", (int)bits);
    if (!n->is_integer) {
        decode_fail(doc, node, expected, "kesirli ya da cok buyuk sayi");
        return 0;
    }
    const int64_t v = n->integer;
    int fits;
    if (is_signed) {
        fits = bits == 64 || (v >= -(INT64_C(1) << (bits - 1)) && v < (INT64_C(1) << (bits - 1)));
    } else {
        fits = v >= 0 && (bits == 64 || v < (INT64_C(1) << bits));
    }
    if (!fits) {
        decode_fail(doc, node, expected, "sinir disinda sayi");
        return 0;
    }
    return v;
}

double marlin_json_decode_float(int64_t d, int64_t node) {
    if (!marlin_json_decode_expect(d, node, JSON_NUMBER)) return 0.0;
    return doc_of(d)->json->nodes[node].number;
}

int32_t marlin_json_decode_bool(int64_t d, int64_t node) {
    if (!marlin_json_decode_expect(d, node, JSON_BOOL)) return 0;
    return doc_of(d)->json->nodes[node].integer != 0;
}

int64_t marlin_json_decode_string(int64_t d, int64_t node) {
    if (!marlin_json_decode_expect(d, node, JSON_STRING)) return marlin_rt_str_from_text("");
    const JsonDoc* doc = doc_of(d)->json;
    return pool_string(doc, doc->nodes[node].offset, doc->nodes[node].length);
}

// ===========================================================================
// to_json: yazıcı
// ===========================================================================

typedef struct {
    char* data;
    size_t length, capacity;
} Writer;

static Writer* writer_of(int64_t handle) { return (Writer*)(intptr_t)handle; }

static void put(Writer* w, const char* bytes, size_t n) {
    if (w->length + n > w->capacity) {
        size_t capacity = w->capacity ? w->capacity : 256;
        while (capacity < w->length + n) capacity *= 2;
        char* grown = realloc(w->data, capacity);
        if (!grown) marlin_rt_panic("to_json: bellek yetersiz");
        w->data = grown;
        w->capacity = capacity;
    }
    memcpy(w->data + w->length, bytes, n);
    w->length += n;
}

static void put_text(Writer* w, const char* text) { put(w, text, strlen(text)); }

static void put_quoted(Writer* w, const char* text, size_t length) {
    const size_t n = json_quote(text, length, NULL);
    char* buffer = malloc(n);
    if (!buffer) marlin_rt_panic("to_json: bellek yetersiz");
    json_quote(text, length, buffer);
    put(w, buffer, n);
    free(buffer);
}

int64_t marlin_json_writer_new(void) {
    Writer* w = calloc(1, sizeof *w);
    if (!w) marlin_rt_panic("to_json: bellek yetersiz");
    return (int64_t)(intptr_t)w;
}

// Yazılanlar yeni bir string olur; yazıcı bırakılır.
int64_t marlin_json_writer_finish(int64_t handle) {
    Writer* w = writer_of(handle);
    const int64_t result = w->length ? marlin_rt_str_new(w->data, (int64_t)w->length)
                                     : marlin_rt_str_from_text("");
    free(w->data);
    free(w);
    return result;
}

// Derleyicinin ürettiği sabit parçalar: "{", "\"ad\":", ",", "]" ...
void marlin_json_write_raw(int64_t handle, int64_t text) {
    put(writer_of(handle), marlin_rt_str_data(text), (size_t)marlin_str_len(text));
}

void marlin_json_write_int(int64_t handle, int64_t value) {
    char text[32];
    snprintf(text, sizeof text, "%lld", (long long)value);
    put_text(writer_of(handle), text);
}

void marlin_json_write_uint(int64_t handle, uint64_t value) {
    char text[32];
    snprintf(text, sizeof text, "%llu", (unsigned long long)value);
    put_text(writer_of(handle), text);
}

// Kayan nokta: geri okunduğunda aynı değeri veren en kısa yazım (print gibi).
// NaN ve sonsuz JSON'da yazılamaz.
static void write_number_string(Writer* w, int64_t text, double value) {
    if (isnan(value) || isinf(value)) {
        marlin_str_release(text);
        marlin_rt_panic("to_json: %s JSON'da yazilamaz", isnan(value) ? "NaN" : "sonsuz");
    }
    put(w, marlin_rt_str_data(text), (size_t)marlin_str_len(text));
    marlin_str_release(text);
}

void marlin_json_write_f64(int64_t handle, double value) {
    write_number_string(writer_of(handle), marlin_str_from_f64(value), value);
}

void marlin_json_write_f32(int64_t handle, float value) {
    write_number_string(writer_of(handle), marlin_str_from_f32(value), value);
}

void marlin_json_write_bool(int64_t handle, int32_t value) {
    put_text(writer_of(handle), value ? "true" : "false");
}

void marlin_json_write_string(int64_t handle, int64_t text) {
    put_quoted(writer_of(handle), marlin_rt_str_data(text), (size_t)marlin_str_len(text));
}

// char tek karakterlik string olarak yazılır.
void marlin_json_write_char(int64_t handle, int32_t value) {
    const char c = (char)value;
    put_quoted(writer_of(handle), &c, 1);
}

// Map anahtarı (string olmayan): JSON nesnesinin anahtarı string olmalıdır,
// bu yüzden tırnak içinde yazılır: {"1": ..., "true": ...}.
// `kind`: 0 işaretli, 1 işaretsiz, 2 char, 3 bool.
void marlin_json_write_key(int64_t handle, int64_t key, int32_t kind) {
    Writer* w = writer_of(handle);
    char text[32];
    switch (kind) {
        case 1: snprintf(text, sizeof text, "%llu", (unsigned long long)key); break;
        case 2: text[0] = (char)key; text[1] = '\0'; break;
        case 3: snprintf(text, sizeof text, "%s", key ? "true" : "false"); break;
        default: snprintf(text, sizeof text, "%lld", (long long)key); break;
    }
    put_quoted(w, text, kind == 2 ? 1 : strlen(text));
}

// Belgedeki bir değeri (json.Value) olduğu gibi yeniden yazar.
static void write_node(Writer* w, const JsonDoc* doc, int64_t node) {
    const JsonNode* n = node_of(doc, node);
    if (!n) {
        put_text(w, "null");
        return;
    }
    switch (n->kind) {
        case JSON_NULL: put_text(w, "null"); return;
        case JSON_BOOL: put_text(w, n->integer ? "true" : "false"); return;
        case JSON_NUMBER: {
            if (n->is_integer) {
                char text[32];
                snprintf(text, sizeof text, "%lld", (long long)n->integer);
                put_text(w, text);
            } else {
                write_number_string(w, marlin_str_from_f64(n->number), n->number);
            }
            return;
        }
        case JSON_STRING: put_quoted(w, doc->pool + n->offset, (size_t)n->length); return;
        case JSON_ARRAY: case JSON_OBJECT: {
            put_text(w, n->kind == JSON_ARRAY ? "[" : "{");
            for (int64_t i = 0; i < n->length; ++i) {
                if (i) put_text(w, ",");
                const int64_t child = doc->children[n->offset + i];
                if (n->kind == JSON_OBJECT) {
                    const JsonNode* c = &doc->nodes[child];
                    put_quoted(w, doc->pool + c->key, (size_t)c->key_length);
                    put_text(w, ":");
                }
                write_node(w, doc, child);
            }
            put_text(w, n->kind == JSON_ARRAY ? "]" : "}");
            return;
        }
        default: put_text(w, "null"); return;
    }
}

void marlin_json_write_value(int64_t handle, int64_t d, int64_t node) {
    write_node(writer_of(handle), doc_of(d)->json, node);
}
