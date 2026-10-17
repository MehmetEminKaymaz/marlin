// std.http.client — HTTP istemcisi (bkz. std/http/client.marl).
//
// libcurl'ü sarar: TLS (HTTPS), yönlendirmeler, sıkıştırılmış yanıtlar,
// parçalı yanıt gövdeleri ve bağlantı ayrıntıları onundur. HTTPS sertifikaları
// doğrulanır (kapatılamaz); Windows'ta sistemin sertifika deposu kullanılır.
//
// Bir istek, marlin tarafına adres olarak (int64) verilen bir "alışveriş"tir:
//
//   begin(yöntem, url) → başlıklar eklenir (add_header) → perform(gövde)
//   → yanıt okunur (ok, error, status, body, header_*) → free
//
// Ağ hataları (adres çözülemedi, bağlantı reddedildi, TLS, zaman aşımı)
// programı durdurmaz: ok yanlış olur, error sebebi söyler. Programı yalnızca
// kullanım hatası durdurur (geçersiz başlık adı / değeri).
//
// İŞ PARÇACIKLARI
//
// Global durum yoktur (libcurl'ün bir kez yapılan genel hazırlığı dışında).
// Her istek kendi curl tanıtıcısını kullanır; görevler aynı anda istek
// gönderebilir.

#include "marlin_rt.h"
#include "http_parser.h"
#include "platform.h"

#include <curl/curl.h>
#include <stdlib.h>
#include <string.h>

enum {
    MAX_RESPONSE_BYTES = 64 * 1024 * 1024,  // daha büyük yanıt hatadır
    MAX_REDIRECTS = 10,
    CONNECT_TIMEOUT_MS = 10 * 1000,
};

typedef struct {
    char* name;
    char* value;
} HeaderPair;

typedef struct {
    // İstek
    char* method;
    char* url;
    struct curl_slist* request_headers;
    int has_expect;            // kullanıcı Expect başlığını kendisi verdi mi?
    long timeout_ms;
    // Yanıt
    int ok;
    char error[CURL_ERROR_SIZE + 64];
    long status;
    char* body;
    size_t body_length, body_capacity;
    int too_large;
    HeaderPair* headers;
    size_t header_count, header_capacity;
} Exchange;

static Exchange* exchange_of(int64_t handle) {
    if (!handle) marlin_rt_panic("http.client: istek gecersiz");
    return (Exchange*)(intptr_t)handle;
}

static char* copy_text(const char* text, size_t length) {
    char* copy = malloc(length + 1);
    if (!copy) marlin_rt_panic("http.client: bellek yetersiz");
    memcpy(copy, text, length);
    copy[length] = '\0';
    return copy;
}

// libcurl'ün genel hazırlığı programda bir kez yapılır (iş parçacıkları
// başlamadan önce olması gerekmez: 0 → 1 → 2 geçişi atomiktir).
static void global_init(void) {
    static int state = 0; // 0 yapılmadı, 1 yapılıyor, 2 hazır
    if (__atomic_load_n(&state, __ATOMIC_ACQUIRE) == 2) return;
    int expected = 0;
    if (__atomic_compare_exchange_n(&state, &expected, 1, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
        if (curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK) marlin_rt_panic("http.client: libcurl baslatilamadi");
        __atomic_store_n(&state, 2, __ATOMIC_RELEASE);
        return;
    }
    while (__atomic_load_n(&state, __ATOMIC_ACQUIRE) != 2) mr_sleep_seconds(0.001);
}

// ---------------------------------------------------------------------------
// libcurl geri çağrıları
// ---------------------------------------------------------------------------

static size_t on_body(char* data, size_t size, size_t count, void* user) {
    Exchange* ex = user;
    const size_t n = size * count;
    if (ex->body_length + n > MAX_RESPONSE_BYTES) {
        ex->too_large = 1;
        return 0; // libcurl aktarımı keser
    }
    if (ex->body_length + n > ex->body_capacity) {
        size_t capacity = ex->body_capacity ? ex->body_capacity : 4096;
        while (capacity < ex->body_length + n) capacity *= 2;
        char* grown = realloc(ex->body, capacity);
        if (!grown) return 0;
        ex->body = grown;
        ex->body_capacity = capacity;
    }
    memcpy(ex->body + ex->body_length, data, n);
    ex->body_length += n;
    return n;
}

static void clear_headers(Exchange* ex) {
    for (size_t i = 0; i < ex->header_count; ++i) {
        free(ex->headers[i].name);
        free(ex->headers[i].value);
    }
    ex->header_count = 0;
}

// Her başlık satırı ayrı gelir. Yönlendirmede ya da "100 Continue"dan sonra
// yeni bir yanıt başlar (durum satırı): önceki yanıtın başlıkları atılır;
// sonunda yalnızca son yanıtınkiler kalır.
static size_t on_header(char* data, size_t size, size_t count, void* user) {
    Exchange* ex = user;
    const size_t n = size * count;
    size_t end = n;
    while (end > 0 && (data[end - 1] == '\r' || data[end - 1] == '\n')) --end;
    if (end >= 5 && memcmp(data, "HTTP/", 5) == 0) {
        clear_headers(ex);
        return n;
    }
    const char* colon = memchr(data, ':', end);
    if (!colon) return n;
    size_t nameLength = (size_t)(colon - data);
    size_t start = nameLength + 1;
    while (start < end && (data[start] == ' ' || data[start] == '\t')) ++start;
    size_t valueEnd = end;
    while (valueEnd > start && (data[valueEnd - 1] == ' ' || data[valueEnd - 1] == '\t')) --valueEnd;

    if (ex->header_count == ex->header_capacity) {
        ex->header_capacity = ex->header_capacity ? ex->header_capacity * 2 : 16;
        HeaderPair* grown = realloc(ex->headers, ex->header_capacity * sizeof *grown);
        if (!grown) return 0;
        ex->headers = grown;
    }
    ex->headers[ex->header_count].name = copy_text(data, nameLength);
    ex->headers[ex->header_count].value = copy_text(data + start, valueEnd - start);
    ++ex->header_count;
    return n;
}

// ---------------------------------------------------------------------------
// İstek
// ---------------------------------------------------------------------------

int64_t marlin_http_client_begin(int64_t method, int64_t url) {
    global_init();
    Exchange* ex = calloc(1, sizeof *ex);
    if (!ex) marlin_rt_panic("http.client: bellek yetersiz");
    const char* m = marlin_rt_str_data(method);
    if (!http_is_token(m, (size_t)marlin_str_len(method)))
        marlin_rt_panic("http.client: gecersiz yontem: \"%s\"", m);
    ex->method = copy_text(m, (size_t)marlin_str_len(method));
    ex->url = copy_text(marlin_rt_str_data(url), (size_t)marlin_str_len(url));
    ex->timeout_ms = 30 * 1000;
    return (int64_t)(intptr_t)ex;
}

// İstek başlığı. Ad ve değer doğrulanır: değerdeki bir satır sonu isteğe
// başka başlıklar sokmanın yolu olurdu (header injection).
void marlin_http_client_add_header(int64_t handle, int64_t name, int64_t value) {
    Exchange* ex = exchange_of(handle);
    const char* n = marlin_rt_str_data(name);
    const char* v = marlin_rt_str_data(value);
    const size_t nl = (size_t)marlin_str_len(name), vl = (size_t)marlin_str_len(value);
    if (!http_is_token(n, nl)) marlin_rt_panic("http.client: gecersiz baslik adi: \"%s\"", n);
    if (!http_is_field_value(v, vl))
        marlin_rt_panic("http.client: '%s' basliginin degeri gecersiz karakter (satir sonu gibi) iceriyor", n);
    HttpSlice slice = {n, nl};
    if (http_equals_ignore_case(slice, "expect")) ex->has_expect = 1;

    char* line = malloc(nl + vl + 3);
    if (!line) marlin_rt_panic("http.client: bellek yetersiz");
    memcpy(line, n, nl);
    // (Boş değer libcurl'de "Ad;" ile yazılır; "Ad:" başlığı silmek demektir.)
    if (vl == 0) {
        line[nl] = ';';
        line[nl + 1] = '\0';
    } else {
        line[nl] = ':';
        line[nl + 1] = ' ';
        memcpy(line + nl + 2, v, vl);
        line[nl + 2 + vl] = '\0';
    }
    struct curl_slist* list = curl_slist_append(ex->request_headers, line);
    free(line);
    if (!list) marlin_rt_panic("http.client: bellek yetersiz");
    ex->request_headers = list;
}

void marlin_http_client_set_timeout(int64_t handle, double seconds) {
    Exchange* ex = exchange_of(handle);
    ex->timeout_ms = seconds <= 0 ? 0 : (long)(seconds * 1000.0 + 0.5);
}

// İsteği gönderir ve yanıtı okur (yanıt gelene ya da zaman aşımına kadar bekler).
void marlin_http_client_perform(int64_t handle, int64_t body) {
    Exchange* ex = exchange_of(handle);
    CURL* curl = curl_easy_init();
    if (!curl) {
        snprintf(ex->error, sizeof ex->error, "libcurl istegi hazirlayamadi");
        return;
    }
    char curlError[CURL_ERROR_SIZE] = "";
    // libcurl büyük gövdelerde "Expect: 100-continue" ekler ve sunucunun
    // cevabını bekler; desteklemeyen sunucuda bu bir saniyelik gecikmedir.
    if (!ex->has_expect) {
        struct curl_slist* list = curl_slist_append(ex->request_headers, "Expect:");
        if (list) ex->request_headers = list;
    }

    curl_easy_setopt(curl, CURLOPT_URL, ex->url);
    curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, curlError);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);  // iş parçacıklarında sinyal kullanılmaz
    // Yalnızca http ve https (file://, ftp:// ... değil), yönlendirmede de.
#if LIBCURL_VERSION_NUM >= 0x075500
    curl_easy_setopt(curl, CURLOPT_PROTOCOLS_STR, "http,https");
    curl_easy_setopt(curl, CURLOPT_REDIR_PROTOCOLS_STR, "http,https");
#else
    curl_easy_setopt(curl, CURLOPT_PROTOCOLS, (long)(CURLPROTO_HTTP | CURLPROTO_HTTPS));
    curl_easy_setopt(curl, CURLOPT_REDIR_PROTOCOLS, (long)(CURLPROTO_HTTP | CURLPROTO_HTTPS));
#endif
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_MAXREDIRS, (long)MAX_REDIRECTS);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, (long)CONNECT_TIMEOUT_MS);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, ex->timeout_ms);
    curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, "");  // desteklenen bütün sıkıştırmalar
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "marlin");
#ifdef _WIN32
    // OpenSSL ile derlenmiş libcurl da Windows'un sertifika deposunu kullansın.
    curl_easy_setopt(curl, CURLOPT_SSL_OPTIONS, (long)CURLSSLOPT_NATIVE_CA);
#endif
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, ex->request_headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, on_body);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, ex);
    curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, on_header);
    curl_easy_setopt(curl, CURLOPT_HEADERDATA, ex);

    // Yöntem: GET ve HEAD gövdesizdir; diğerleri gövdeyle (boş da olabilir).
    if (strcmp(ex->method, "HEAD") == 0) {
        curl_easy_setopt(curl, CURLOPT_NOBODY, 1L);
    } else if (strcmp(ex->method, "GET") != 0 || marlin_str_len(body) > 0) {
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, marlin_rt_str_data(body));
        curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE_LARGE, (curl_off_t)marlin_str_len(body));
        curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, ex->method);
    }

    const CURLcode result = curl_easy_perform(curl);
    if (result == CURLE_OK) {
        ex->ok = 1;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &ex->status);
    } else if (ex->too_large) {
        snprintf(ex->error, sizeof ex->error, "yanit cok buyuk (en fazla %d MB)", MAX_RESPONSE_BYTES / (1024 * 1024));
    } else {
        snprintf(ex->error, sizeof ex->error, "%s", curlError[0] ? curlError : curl_easy_strerror(result));
    }
    if (!ex->ok) {
        clear_headers(ex);
        ex->body_length = 0;
    }
    curl_easy_cleanup(curl);
}

// ---------------------------------------------------------------------------
// Yanıt
// ---------------------------------------------------------------------------

int32_t marlin_http_client_ok(int64_t h) { return exchange_of(h)->ok; }
int64_t marlin_http_client_error(int64_t h) { return marlin_rt_str_from_text(exchange_of(h)->error); }
int32_t marlin_http_client_status(int64_t h) { return (int32_t)exchange_of(h)->status; }

int64_t marlin_http_client_body(int64_t h) {
    Exchange* ex = exchange_of(h);
    return ex->body_length ? marlin_rt_str_new(ex->body, (int64_t)ex->body_length) : marlin_rt_str_from_text("");
}

int64_t marlin_http_client_header_count(int64_t h) { return (int64_t)exchange_of(h)->header_count; }

static const HeaderPair* header_at(Exchange* ex, int64_t index) {
    if (index < 0 || (size_t)index >= ex->header_count)
        marlin_rt_panic("http.client: baslik indeksi sinir disi: %lld", (long long)index);
    return &ex->headers[index];
}

int64_t marlin_http_client_header_name(int64_t h, int64_t i) {
    return marlin_rt_str_from_text(header_at(exchange_of(h), i)->name);
}

int64_t marlin_http_client_header_value(int64_t h, int64_t i) {
    return marlin_rt_str_from_text(header_at(exchange_of(h), i)->value);
}

void marlin_http_client_free(int64_t h) {
    if (!h) return;
    Exchange* ex = (Exchange*)(intptr_t)h;
    clear_headers(ex);
    free(ex->headers);
    free(ex->body);
    free(ex->method);
    free(ex->url);
    curl_slist_free_all(ex->request_headers);
    free(ex);
}
