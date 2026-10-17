// std.http — HTTP sunucusunun soket tarafı (bkz. std/http.marl).
//
// Sunucu ve istek, marlin tarafına adres olarak (int64) verilir. Akış:
//
//   listen(port)           dinleyen soket
//   accept(sunucu)         sıradaki bağlantıyı kabul eder, isteğin tamamını
//                          (başlık + gövde) okur ve ayrıştırır (http_parser.c).
//                          Geçersiz isteklere hata koduyla kendisi yanıt verir
//                          ve sıradakine geçer; yalnızca geçerli istek döner.
//   method / path / ...    isteğin parçaları (yeni stringler)
//   respond_header         yanıta başlık ekler
//   respond(istek, kod, gövde)   yanıtı gönderir, bağlantıyı kapatır, isteği bırakır
//
// Her yanıttan sonra bağlantı kapanır (Connection: close).
//
// İŞ PARÇACIKLARI
//
// Bu dosyada global durum yoktur: her şey sunucu ve istek nesnelerinin
// içindedir. Aynı sunucudan birden çok iş parçacığı aynı anda accept
// çağırabilir (çekirdek bağlantıları aralarında paylaştırır); bir istek ise
// yalnızca onu alan iş parçacığında kullanılır. Son hata mesajı iş
// parçacığına özeldir.
//
// Soket çağrıları platform katmanındadır (platform.h): aynı kod Linux, macOS
// ve Windows'ta derlenir.

#include "marlin_rt.h"
#include "http_parser.h"
#include "platform.h"

#include <stdlib.h>
#include <string.h>

enum {
    MAX_HEAD_BYTES = 64 * 1024,        // başlık bundan büyükse 431
    MAX_BODY_BYTES = 8 * 1024 * 1024,  // gövde bundan büyükse 413
    IO_TIMEOUT_SECONDS = 10,           // yavaş istemci sunucuyu sonsuza dek bekletemez
};

typedef struct {
    mr_socket fd;
} Server;

typedef struct {
    mr_socket fd;
    char* data;         // isteğin baytları: başlık + gövde
    HttpHead head;       // `data`nın içini gösterir
    // Yanıt başlıkları (respond_header ile eklenenler), "ad: değer\r\n" olarak.
    char* headers;
    size_t headers_length, headers_capacity;
    int has_content_type;
} Request;

static _Thread_local char last_error[256];

static Server* server_of(int64_t handle, const char* operation) {
    if (!handle) marlin_rt_panic("http.%s: sunucu acik degil (listen basarisiz olmus; once 'ok' alanina bakin)", operation);
    return (Server*)(intptr_t)handle;
}

static Request* request_of(int64_t handle, const char* operation) {
    if (!handle) marlin_rt_panic("http.%s: istek gecersiz (yaniti zaten gonderilmis olabilir)", operation);
    return (Request*)(intptr_t)handle;
}

static int64_t slice_string(HttpSlice s) { return marlin_rt_str_new(s.data, (int64_t)s.length); }

// ---------------------------------------------------------------------------
// Soket yardımcıları
// ---------------------------------------------------------------------------

// Geçersiz isteğe kısa bir hata yanıtı.
static void send_error(mr_socket fd, int status) {
    const char* reason = http_reason_phrase(status);
    char response[256];
    int n = snprintf(response, sizeof response,
                     "HTTP/1.1 %d %s\r\n"
                     "Content-Type: text/plain; charset=utf-8\r\n"
                     "Content-Length: %zu\r\n"
                     "Connection: close\r\n"
                     "\r\n"
                     "%s\n",
                     status, reason, strlen(reason) + 1, reason);
    mr_socket_send_all(fd, response, (size_t)n);
}

static int expects_continue(const HttpHead* head) {
    for (size_t i = 0; i < head->header_count; ++i)
        if (http_equals_ignore_case(head->header_names[i], "expect") &&
            http_equals_ignore_case(head->header_values[i], "100-continue"))
            return 1;
    return 0;
}

// Bağlantıdan bir isteğin tamamını okur. Geçersizse hata yanıtını gönderip
// bağlantıyı kapatır ve NULL döner.
static Request* read_request(mr_socket fd) {
    size_t capacity = 4096, length = 0;
    char* data = malloc(capacity);
    HttpHead head;
    int status = 0;

    // 1) Başlık: boş satır gelene kadar oku.
    for (;;) {
        HttpParseResult result = http_parse_head(data, length, &head, &status);
        if (result == HTTP_PARSE_OK) break;
        if (result == HTTP_PARSE_ERROR) goto reject;
        if (length == MAX_HEAD_BYTES) {
            status = 431;
            goto reject;
        }
        if (length == capacity) {
            capacity = capacity * 2 > MAX_HEAD_BYTES ? MAX_HEAD_BYTES : capacity * 2;
            data = realloc(data, capacity);
        }
        int64_t n = mr_socket_recv(fd, data + length, capacity - length);
        if (n <= 0) {
            // Hiçbir şey göndermeden kapanan bağlantı sessizce bırakılır;
            // isteğin yarısında zaman aşımı olduysa 408.
            if (length > 0 && n == MR_RECV_TIMEOUT) {
                status = 408;
                goto reject;
            }
            goto drop;
        }
        length += (size_t)n;
    }

    // 2) Gövde: Content-Length kadar bayt.
    if (head.content_length > MAX_BODY_BYTES) {
        status = 413;
        goto reject;
    }
    const size_t total = head.head_length + (size_t)head.content_length;
    if (total > length && expects_continue(&head))
        mr_socket_send_all(fd, "HTTP/1.1 100 Continue\r\n\r\n", 25);
    if (total + 1 > capacity) {
        capacity = total + 1;
        data = realloc(data, capacity);
    }
    while (length < total) {
        int64_t n = mr_socket_recv(fd, data + length, total - length);
        if (n <= 0) {
            if (n == MR_RECV_TIMEOUT) {
                status = 408;
                goto reject;
            }
            goto drop;
        }
        length += (size_t)n;
    }

    // Tampon yer değiştirmiş olabilir: parçaların yeni yeri için yeniden ayrıştır.
    http_parse_head(data, total, &head, &status);

    Request* request = calloc(1, sizeof *request);
    request->fd = fd;
    request->data = data;
    request->head = head;
    return request;

reject:
    send_error(fd, status);
drop:
    free(data);
    mr_socket_close(fd);
    return NULL;
}

// ---------------------------------------------------------------------------
// Sunucu
// ---------------------------------------------------------------------------

// Bütün ağ arayüzlerinde `port`u dinler. Başarısızsa 0 döner (sebebi: c_error);
// or_die ise programı mesajla durdurur.
int64_t marlin_http_listen(int32_t port, int32_t or_die) {
    last_error[0] = '\0';
    if (port < 0 || port > 65535) {
        snprintf(last_error, sizeof last_error, "gecersiz port: %d (0..65535 olmali)", port);
    } else {
        char reason[200];
        mr_socket fd = mr_tcp_listen(port, 128, reason, sizeof reason);
        if (fd != MR_NO_SOCKET) {
            Server* server = malloc(sizeof *server);
            server->fd = fd;
            return (int64_t)(intptr_t)server;
        }
        snprintf(last_error, sizeof last_error, "port %d dinlenemedi: %s", port, reason);
    }
    if (or_die) marlin_rt_panic("http: %s", last_error);
    return 0;
}

// Bu iş parçacığında son listen hatasının açıklaması.
int64_t marlin_http_error(void) { return marlin_rt_str_from_text(last_error); }

// Sıradaki geçerli isteği bekler.
int64_t marlin_http_accept(int64_t handle) {
    Server* server = server_of(handle, "next");
    for (;;) {
        int transient = 0;
        mr_socket fd = mr_tcp_accept(server->fd, &transient);
        if (fd == MR_NO_SOCKET) {
            // Geçici durumlar: bağlantı kabul edilmeden koptu, sinyal geldi,
            // dosya tanıtıcısı ya da bellek bir an için tükendi.
            if (transient) {
                mr_sleep_seconds(0.01);
                continue;
            }
            char reason[200];
            mr_socket_error_text(reason, sizeof reason);
            marlin_rt_panic("http: baglanti kabul edilemedi (%s)", reason);
        }
        mr_socket_set_timeout(fd, IO_TIMEOUT_SECONDS);

        Request* request = read_request(fd);
        if (request) return (int64_t)(intptr_t)request;
    }
}

void marlin_http_close(int64_t handle) {
    if (!handle) return;
    Server* server = (Server*)(intptr_t)handle;
    mr_socket_close(server->fd);
    free(server);
}

// ---------------------------------------------------------------------------
// İstek
// ---------------------------------------------------------------------------

int64_t marlin_http_method(int64_t r) { return slice_string(request_of(r, "method")->head.method); }
int64_t marlin_http_path(int64_t r)   { return slice_string(request_of(r, "path")->head.path); }
int64_t marlin_http_query(int64_t r)  { return slice_string(request_of(r, "query")->head.query); }

int64_t marlin_http_body(int64_t r) {
    Request* request = request_of(r, "body");
    return marlin_rt_str_new(request->data + request->head.head_length, request->head.content_length);
}

int64_t marlin_http_header_count(int64_t r) { return (int64_t)request_of(r, "header")->head.header_count; }

static size_t header_index(Request* request, int64_t index) {
    if (index < 0 || (size_t)index >= request->head.header_count)
        marlin_rt_panic("http: baslik indeksi sinir disi: %lld (baslik sayisi %zu)",
                        (long long)index, request->head.header_count);
    return (size_t)index;
}

int64_t marlin_http_header_name(int64_t r, int64_t index) {
    Request* request = request_of(r, "header");
    return slice_string(request->head.header_names[header_index(request, index)]);
}

int64_t marlin_http_header_value(int64_t r, int64_t index) {
    Request* request = request_of(r, "header");
    return slice_string(request->head.header_values[header_index(request, index)]);
}

// ---------------------------------------------------------------------------
// Yanıt
// ---------------------------------------------------------------------------

static void append_header(Request* request, const char* text, size_t length) {
    if (request->headers_length + length > request->headers_capacity) {
        size_t capacity = request->headers_capacity ? request->headers_capacity : 256;
        while (capacity < request->headers_length + length) capacity *= 2;
        request->headers = realloc(request->headers, capacity);
        request->headers_capacity = capacity;
    }
    memcpy(request->headers + request->headers_length, text, length);
    request->headers_length += length;
}

// Yanıta bir başlık ekler. Ad ve değer doğrulanır: değerdeki bir satır sonu,
// yanıta istemcinin yazdığı başlıkları sokmanın yolu olurdu (header injection).
void marlin_http_respond_header(int64_t r, int64_t name, int64_t value) {
    Request* request = request_of(r, "respond");
    const char* n = marlin_rt_str_data(name);
    const char* v = marlin_rt_str_data(value);
    const size_t nl = (size_t)marlin_str_len(name), vl = (size_t)marlin_str_len(value);
    if (!http_is_token(n, nl)) marlin_rt_panic("http: gecersiz yanit basligi adi: \"%s\"", n);
    if (!http_is_field_value(v, vl))
        marlin_rt_panic("http: '%s' basliginin degeri gecersiz karakter (satir sonu gibi) iceriyor", n);

    HttpSlice slice = {n, nl};
    if (http_equals_ignore_case(slice, "content-length") || http_equals_ignore_case(slice, "connection") ||
        http_equals_ignore_case(slice, "transfer-encoding"))
        marlin_rt_panic("http: '%s' basligi otomatik eklenir; elle verilemez", n);
    if (http_equals_ignore_case(slice, "content-type")) request->has_content_type = 1;

    append_header(request, n, nl);
    append_header(request, ": ", 2);
    append_header(request, v, vl);
    append_header(request, "\r\n", 2);
}

// Yanıtı gönderir, bağlantıyı kapatır ve isteği bırakır.
void marlin_http_respond(int64_t r, int32_t status, int64_t body) {
    Request* request = request_of(r, "respond");
    if (status < 200 || status > 999)
        marlin_rt_panic("http: gecersiz durum kodu: %d (200..999 olmali)", status);

    // 204 ve 304 yanıtlarının gövdesi olmaz; HEAD isteğine gövde gönderilmez
    // ama uzunluğu GET'teki gibi bildirilir (RFC 9110 §9.3.2, §15.3.5, §15.4.5).
    const int no_body_status = status == 204 || status == 304;
    const int is_head = http_equals_ignore_case(request->head.method, "HEAD");
    const size_t body_length = no_body_status ? 0 : (size_t)marlin_str_len(body);

    char line[128];
    int n = snprintf(line, sizeof line, "HTTP/1.1 %d %s\r\n", status, http_reason_phrase(status));
    size_t capacity = (size_t)n + request->headers_length + 128;
    char* head = malloc(capacity);
    size_t length = 0;
    memcpy(head, line, (size_t)n);
    length += (size_t)n;
    if (request->headers_length) memcpy(head + length, request->headers, request->headers_length);
    length += request->headers_length;
    if (!request->has_content_type && body_length > 0)
        length += (size_t)snprintf(head + length, capacity - length, "Content-Type: text/plain; charset=utf-8\r\n");
    if (!no_body_status)
        length +=(size_t)snprintf(head + length, capacity - length, "Content-Length: %zu\r\n", body_length);
    length += (size_t)snprintf(head + length, capacity - length, "Connection: close\r\n\r\n");

    if (mr_socket_send_all(request->fd, head, length) && !is_head && body_length > 0)
        mr_socket_send_all(request->fd, marlin_rt_str_data(body), body_length);
    free(head);

    mr_socket_close(request->fd);
    free(request->headers);
    free(request->data);
    free(request);
}

// ---------------------------------------------------------------------------
// Kodlama
// ---------------------------------------------------------------------------

static int hex_value(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

// %XX dizilerini baytlara çevirir; form ise '+' boşluk olur (sorgu ve form
// gövdesinde böyledir, yolda değil). Geçersiz bir % dizisi olduğu gibi kalır.
int64_t marlin_http_decode(int64_t text, int32_t form) {
    const char* s = marlin_rt_str_data(text);
    const int64_t length = marlin_str_len(text);
    char* out = malloc((size_t)length + 1);
    int64_t n = 0;
    for (int64_t i = 0; i < length; ++i) {
        if (s[i] == '%' && i + 2 < length && hex_value(s[i + 1]) >= 0 && hex_value(s[i + 2]) >= 0) {
            out[n++] = (char)(hex_value(s[i + 1]) * 16 + hex_value(s[i + 2]));
            i += 2;
        } else if (form && s[i] == '+') {
            out[n++] = ' ';
        } else {
            out[n++] = s[i];
        }
    }
    const int64_t result = marlin_rt_str_new(out, n);
    free(out);
    return result;
}
