// std.http — HTTP sunucusunun soket tarafı (bkz. std/http.marl).
//
// Sunucu ve istek, marlin tarafına adres olarak (int64) verilir. Akış:
//
//   listen(port)           dinleyen soket
//   accept(sunucu)         sıradaki TAMAMLANMIŞ isteği döndürür (bkz. OLAY DÖNGÜSÜ).
//                          Geçersiz isteklere hata koduyla kendisi yanıt verir;
//                          yalnızca geçerli istek döner.
//   method / path / ...    isteğin parçaları (yeni stringler)
//   respond_header         yanıta başlık ekler
//   respond(istek, kod, gövde)   yanıtı gönderir, isteği bırakır; bağlantı açık
//                          kalacaksa sunucuya geri verilir, değilse kapanır
//
// BAĞLANTILAR (keep-alive, RFC 9112 §9.3)
//
// Bir bağlantı üzerinden art arda birçok istek gelebilir. HTTP/1.1'de bağlantı
// varsayılan olarak açık kalır; HTTP/1.0'da yalnızca istemci
// "Connection: keep-alive" derse. İstemci "Connection: close" derse, bağlantı
// MAX_REQUESTS_PER_CONNECTION isteğe ulaşırsa ya da istek geçersizse yanıttan
// sonra kapanır. Yanıt bunu "Connection: keep-alive" / "close" ile bildirir.
//
// İstemci bir sonraki isteği öncekinin yanıtını beklemeden de gönderebilir
// (pipelining): fazladan okunan baytlar bağlantının tamponunda kalır ve sıradaki
// istek olarak işlenir.
//
// OLAY DÖNGÜSÜ
//
// Açık bağlantılar sunucudadır; soketleri engellemeyen kiptedir. accept,
// dinleyen soketi ve bütün bağlantıları poll ile (Windows'ta WSAPoll) birlikte
// bekler; gelen baytları bağlantının tamponuna ekler ve yalnızca tamamlanmış
// (başlığı ve gövdesi gelmiş) bir isteği döndürür. Böylece yavaş bir istemci
// diğerlerini bekletmez.
//
// Zaman aşımları:
//   * İsteğin yarısında bekleyen bağlantı: REQUEST_TIMEOUT_SECONDS sonra 408.
//   * Boşta bekleyen (yeni ya da yanıtı gönderilmiş) bağlantı: IDLE_TIMEOUT_SECONDS
//     sonra sessizce kapanır.
//
// İŞ PARÇACIKLARI
//
// Global durum yoktur. accept'i birden çok iş parçacığı çağırabilir: biri
// beklerken diğerleri sırasını bekler (next_lock). İstek, onu alan iş
// parçacığından başka birinde yanıtlanabilir (serve_concurrent: görevler);
// respond bağlantıyı "dönenler" kuyruğuna koyar ve poll'da bekleyeni bir soket
// çiftiyle (wake) uyandırır. Son listen hatası iş parçacığına özeldir.
//
// Soket çağrıları platform katmanındadır (platform.h): aynı kod Linux, macOS
// ve Windows'ta derlenir.

#include "marlin_rt.h"
#include "http_parser.h"
#include "platform.h"

#include <stdlib.h>
#include <string.h>

enum {
    MAX_HEAD_BYTES = 64 * 1024,           // başlık bundan büyükse 431
    MAX_BODY_BYTES = 8 * 1024 * 1024,     // gövde bundan büyükse 413
    REQUEST_TIMEOUT_SECONDS = 10,         // yarım istek en fazla bu kadar beklenir
    IDLE_TIMEOUT_SECONDS = 5,             // boştaki bağlantı en fazla bu kadar açık kalır
    SEND_TIMEOUT_SECONDS = 10,            // yanıtı almayan istemci en fazla bu kadar beklenir
    MAX_REQUESTS_PER_CONNECTION = 1000,
    MAX_CONNECTIONS = 1000,               // bunu aşınca yeni bağlantı kabul edilmez (bekler)
};

typedef struct {
    mr_socket fd;
    char* data;             // gelmiş ama henüz işlenmemiş baytlar
    size_t length, capacity;
    double deadline;        // bu ana kadar ilerleme olmazsa kapanır (bkz. zaman aşımları)
    int partial;            // tamponda yarım bir istek var mı?
    int continue_sent;      // bu istek için "100 Continue" gönderildi mi?
    int served;             // bu bağlantıda yanıtlanan istek sayısı
    int eof;                // istemci yazma yönünü kapattı: tampondakiler yanıtlanır, sonra kapanır
} Connection;

typedef struct {
    Connection** items;
    size_t count, capacity;
} ConnectionList;

typedef struct {
    mr_socket listener;
    mr_socket wake[2];        // respond wake[1]'e yazar; accept wake[0]'ı da bekler
    mr_mutex next_lock;       // accept'i aynı anda tek iş parçacığı yürütür
    ConnectionList open;      // bekleyen bağlantılar (yalnızca next_lock tutulurken)
    mr_mutex returned_lock;
    ConnectionList returned;  // yanıtı gönderilip geri verilen bağlantılar
} Server;

typedef struct {
    Server* server;
    Connection* conn;
    HttpHead head;          // conn->data'nın içini gösterir
    size_t total;           // isteğin bayt sayısı (başlık + gövde)
    int keep_alive;         // istemci bağlantının açık kalmasını kabul ediyor mu?
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

static void* checked_realloc(void* p, size_t size) {
    void* q = realloc(p, size);
    if (!q) marlin_rt_panic("http: bellek yetersiz");
    return q;
}

// ---------------------------------------------------------------------------
// Bağlantılar
// ---------------------------------------------------------------------------

static void list_push(ConnectionList* list, Connection* conn) {
    if (list->count == list->capacity) {
        list->capacity = list->capacity ? list->capacity * 2 : 16;
        list->items = checked_realloc(list->items, list->capacity * sizeof *list->items);
    }
    list->items[list->count++] = conn;
}

// i. elemanı çıkarır (sıra önemli değil: sonuncuyla yer değiştirir).
static void list_remove(ConnectionList* list, size_t i) { list->items[i] = list->items[--list->count]; }

static void close_connection(Connection* conn) {
    mr_socket_close(conn->fd);
    free(conn->data);
    free(conn);
}

// Geçersiz isteğe kısa bir hata yanıtı; ardından bağlantı kapanır.
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
    mr_socket_send_all(fd, response, (size_t)n, SEND_TIMEOUT_SECONDS);
}

static int expects_continue(const HttpHead* head) {
    for (size_t i = 0; i < head->header_count; ++i)
        if (http_equals_ignore_case(head->header_names[i], "expect") &&
            http_equals_ignore_case(head->header_values[i], "100-continue"))
            return 1;
    return 0;
}

// "Connection" başlığındaki virgülle ayrılmış seçeneklerden biri `token` mı?
static int has_connection_token(const HttpHead* head, const char* token) {
    const size_t n = strlen(token);
    for (size_t i = 0; i < head->header_count; ++i) {
        if (!http_equals_ignore_case(head->header_names[i], "connection")) continue;
        const HttpSlice v = head->header_values[i];
        size_t start = 0;
        while (start < v.length) {
            size_t end = start;
            while (end < v.length && v.data[end] != ',') ++end;
            size_t a = start, b = end;
            while (a < b && (v.data[a] == ' ' || v.data[a] == '\t')) ++a;
            while (b > a && (v.data[b - 1] == ' ' || v.data[b - 1] == '\t')) --b;
            HttpSlice part = {v.data + a, b - a};
            if (part.length == n && http_equals_ignore_case(part, token)) return 1;
            start = end + 1;
        }
    }
    return 0;
}

// RFC 9112 §9.3: close varsa kapanır; HTTP/1.1'de varsayılan açık, HTTP/1.0'da
// yalnızca keep-alive istenirse.
static int wants_keep_alive(const HttpHead* head) {
    if (has_connection_token(head, "close")) return 0;
    return head->minor_version >= 1 || has_connection_token(head, "keep-alive");
}

typedef enum { TAKE_NONE, TAKE_READY, TAKE_REJECT } TakeResult;

// Tamponda tamamlanmış bir istek var mı? READY: `*out` doldu; REJECT: `*status`
// hata kodu (bağlantı kapanmalı); NONE: daha çok bayt gerekiyor.
static TakeResult take_request(Server* server, Connection* conn, Request** out, int* status) {
    if (conn->length == 0) return TAKE_NONE;
    HttpHead head;
    HttpParseResult result = http_parse_head(conn->data, conn->length, &head, status);
    if (result == HTTP_PARSE_ERROR) return TAKE_REJECT;
    if (result == HTTP_PARSE_INCOMPLETE) {
        if (conn->length >= MAX_HEAD_BYTES) {
            *status = 431;
            return TAKE_REJECT;
        }
        return TAKE_NONE;
    }
    if (head.content_length > MAX_BODY_BYTES) {
        *status = 413;
        return TAKE_REJECT;
    }
    const size_t total = head.head_length + (size_t)head.content_length;
    if (conn->length < total) {
        if (!conn->continue_sent && expects_continue(&head)) {
            mr_socket_send_all(conn->fd, "HTTP/1.1 100 Continue\r\n\r\n", 25, SEND_TIMEOUT_SECONDS);
            conn->continue_sent = 1;
        }
        return TAKE_NONE;
    }

    Request* request = calloc(1, sizeof *request);
    if (!request) marlin_rt_panic("http: bellek yetersiz");
    request->server = server;
    request->conn = conn;
    request->head = head; // (bağlantı istek yanıtlanana kadar okunmaz: tampon yerinde kalır)
    request->total = total;
    request->keep_alive = wants_keep_alive(&head);
    *out = request;
    return TAKE_READY;
}

// Sokette bekleyen bütün baytları tampona ekler. Hata olduysa 0. İstemci
// yazma yönünü kapattıysa conn->eof olur (tampondaki istek yine yanıtlanır).
static int read_available(Connection* conn, double now) {
    for (;;) {
        if (conn->length == conn->capacity) {
            // Sınır: en büyük başlık + en büyük gövde (aşarsa take_request reddeder).
            if (conn->capacity >= (size_t)MAX_HEAD_BYTES + MAX_BODY_BYTES) return 1;
            conn->capacity = conn->capacity ? conn->capacity * 2 : 4096;
            conn->data = checked_realloc(conn->data, conn->capacity);
        }
        int64_t n = mr_socket_recv(conn->fd, conn->data + conn->length, conn->capacity - conn->length);
        if (n == MR_RECV_WOULD_BLOCK) return 1;
        if (n == 0) {
            conn->eof = 1;
            return 1;
        }
        if (n < 0) return 0;
        if (!conn->partial) {
            // Yeni bir isteğin ilk baytları: istek için süre başlar.
            conn->partial = 1;
            conn->deadline = now + REQUEST_TIMEOUT_SECONDS;
        }
        conn->length += (size_t)n;
    }
}

static void accept_connections(Server* server, double now) {
    while (server->open.count < MAX_CONNECTIONS) {
        int transient = 0;
        mr_socket fd = mr_tcp_accept(server->listener, &transient);
        if (fd == MR_NO_SOCKET) {
            if (transient == MR_ACCEPT_NONE) return;
            if (transient == MR_ACCEPT_EXHAUSTED) {
                // Tanıtıcı ya da bellek bir an için tükendi; bekleyenler sonra alınır.
                mr_sleep_seconds(0.01);
                return;
            }
            char reason[200];
            mr_socket_error_text(reason, sizeof reason);
            marlin_rt_panic("http: baglanti kabul edilemedi (%s)", reason);
        }
        mr_socket_set_nonblocking(fd);
        Connection* conn = calloc(1, sizeof *conn);
        if (!conn) marlin_rt_panic("http: bellek yetersiz");
        conn->fd = fd;
        conn->deadline = now + REQUEST_TIMEOUT_SECONDS;
        list_push(&server->open, conn);
    }
}

// respond'un geri verdiği bağlantılar bekleyenlere katılır; uyandırma baytları atılır.
static void collect_returned(Server* server) {
    char drain[64];
    while (mr_socket_recv(server->wake[0], drain, sizeof drain) > 0) {}
    mr_mutex_lock(&server->returned_lock);
    for (size_t i = 0; i < server->returned.count; ++i) list_push(&server->open, server->returned.items[i]);
    server->returned.count = 0;
    mr_mutex_unlock(&server->returned_lock);
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
            Server* server = calloc(1, sizeof *server);
            if (!server || !mr_socket_pair(server->wake))
                marlin_rt_panic("http: sunucu icin kaynak ayrilamadi");
            server->listener = fd;
            mr_socket_set_nonblocking(fd);
            mr_socket_set_nonblocking(server->wake[0]);
            mr_socket_set_nonblocking(server->wake[1]);
            mr_mutex_init(&server->next_lock);
            mr_mutex_init(&server->returned_lock);
            return (int64_t)(intptr_t)server;
        }
        snprintf(last_error, sizeof last_error, "port %d dinlenemedi: %s", port, reason);
    }
    if (or_die) marlin_rt_panic("http: %s", last_error);
    return 0;
}

// Bu iş parçacığında son listen hatasının açıklaması.
int64_t marlin_http_error(void) { return marlin_rt_str_from_text(last_error); }

// Sıradaki geçerli isteği bekler (bkz. OLAY DÖNGÜSÜ).
int64_t marlin_http_accept(int64_t handle) {
    Server* server = server_of(handle, "next");
    mr_mutex_lock(&server->next_lock);
    ConnectionList* open = &server->open;
    mr_pollfd small[64];
    mr_pollfd* fds = small;
    size_t fdsCapacity = 64;

    for (;;) {
        collect_returned(server);
        const double now = mr_monotonic_seconds();

        // 1) Tamponunda tamamlanmış istek olan bağlantı (pipelining, ya da az
        //    önce gelen baytlar) — ya da süresi dolan bağlantı.
        double nearest = -1;
        for (size_t i = 0; i < open->count;) {
            Connection* conn = open->items[i];
            Request* request = NULL;
            int status = 0;
            const TakeResult taken = take_request(server, conn, &request, &status);
            if (taken == TAKE_READY) {
                list_remove(open, i);
                if (fds != small) free(fds);
                mr_mutex_unlock(&server->next_lock);
                return (int64_t)(intptr_t)request;
            }
            if (taken == TAKE_REJECT) {
                send_error(conn->fd, status);
                list_remove(open, i);
                close_connection(conn);
                continue;
            }
            // İstemci kapattı ve tamamlanmış istek kalmadı.
            if (conn->eof) {
                list_remove(open, i);
                close_connection(conn);
                continue;
            }
            if (now >= conn->deadline) {
                // Yarım istek: 408; boştaki bağlantı sessizce kapanır.
                if (conn->partial) send_error(conn->fd, 408);
                list_remove(open, i);
                close_connection(conn);
                continue;
            }
            if (nearest < 0 || conn->deadline < nearest) nearest = conn->deadline;
            ++i;
        }

        // 2) Uyandırma soketi, dinleyen soket ve bağlantılar birlikte beklenir.
        const size_t count = 2 + open->count;
        if (count > fdsCapacity) {
            fdsCapacity = count * 2;
            if (fds != small) free(fds);
            fds = malloc(fdsCapacity * sizeof *fds);
            if (!fds) marlin_rt_panic("http: bellek yetersiz");
        }
        fds[0] = (mr_pollfd){server->wake[0], MR_POLL_IN, 0};
        // (Bağlantı sınırındayken yeni bağlantılar kabul edilmez; işletim
        // sisteminin kuyruğunda beklerler.)
        fds[1] = (mr_pollfd){server->listener, open->count < MAX_CONNECTIONS ? MR_POLL_IN : 0, 0};
        for (size_t i = 0; i < open->count; ++i) fds[2 + i] = (mr_pollfd){open->items[i]->fd, MR_POLL_IN, 0};
        const int timeout = nearest < 0 ? -1 : (int)((nearest - now) * 1000) + 1;
        if (mr_poll(fds, count, timeout) < 0) {
            char reason[200];
            mr_socket_error_text(reason, sizeof reason);
            marlin_rt_panic("http: poll basarisiz (%s)", reason);
        }

        const double after = mr_monotonic_seconds();
        // Bağlantılar (sondan başa: kapananlar listeden çıkarılırken sıra bozulmasın)
        const size_t polled = open->count;
        for (size_t i = polled; i-- > 0;) {
            if (!(fds[2 + i].revents & (MR_POLL_IN | MR_POLL_ERR))) continue;
            Connection* conn = open->items[i];
            if (!read_available(conn, after)) {
                list_remove(open, i);
                close_connection(conn);
            }
        }
        if (fds[1].revents & MR_POLL_IN) accept_connections(server, after);
    }
}

void marlin_http_close(int64_t handle) {
    if (!handle) return;
    Server* server = (Server*)(intptr_t)handle;
    collect_returned(server);
    for (size_t i = 0; i < server->open.count; ++i) close_connection(server->open.items[i]);
    free(server->open.items);
    free(server->returned.items);
    mr_socket_close(server->listener);
    mr_socket_close(server->wake[0]);
    mr_socket_close(server->wake[1]);
    mr_mutex_destroy(&server->next_lock);
    mr_mutex_destroy(&server->returned_lock);
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
    return marlin_rt_str_new(request->conn->data + request->head.head_length, request->head.content_length);
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
        request->headers = checked_realloc(request->headers, capacity);
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

// Yanıtı gönderir ve isteği bırakır. Bağlantı açık kalacaksa sunucuya geri
// verilir (pipelining ile gelmiş sonraki baytlarıyla birlikte), değilse kapanır.
void marlin_http_respond(int64_t r, int32_t status, int64_t body) {
    Request* request = request_of(r, "respond");
    Connection* conn = request->conn;
    Server* server = request->server;
    if (status < 200 || status > 999)
        marlin_rt_panic("http: gecersiz durum kodu: %d (200..999 olmali)", status);

    // 204 ve 304 yanıtlarının gövdesi olmaz; HEAD isteğine gövde gönderilmez
    // ama uzunluğu GET'teki gibi bildirilir (RFC 9110 §9.3.2, §15.3.5, §15.4.5).
    const int no_body_status = status == 204 || status == 304;
    const int is_head = http_equals_ignore_case(request->head.method, "HEAD");
    const size_t body_length = no_body_status ? 0 : (size_t)marlin_str_len(body);
    // İstemci yazma yönünü kapattıysa yalnızca tamponda bekleyen (pipelining)
    // istekler için açık kalır; arkasından yeni bayt gelmez.
    const int leftover = conn->length > request->total;
    int keep = request->keep_alive && (!conn->eof || leftover) &&
               conn->served + 1 < MAX_REQUESTS_PER_CONNECTION;

    char line[128];
    int n = snprintf(line, sizeof line, "HTTP/1.1 %d %s\r\n", status, http_reason_phrase(status));
    size_t capacity = (size_t)n + request->headers_length + 128;
    char* head = malloc(capacity);
    if (!head) marlin_rt_panic("http: bellek yetersiz");
    size_t length = 0;
    memcpy(head, line, (size_t)n);
    length += (size_t)n;
    if (request->headers_length) memcpy(head + length, request->headers, request->headers_length);
    length += request->headers_length;
    if (!request->has_content_type && body_length > 0)
        length += (size_t)snprintf(head + length, capacity - length, "Content-Type: text/plain; charset=utf-8\r\n");
    if (!no_body_status)
        length += (size_t)snprintf(head + length, capacity - length, "Content-Length: %zu\r\n", body_length);
    length += (size_t)snprintf(head + length, capacity - length, "Connection: %s\r\n\r\n",
                               keep ? "keep-alive" : "close");

    int sent = mr_socket_send_all(conn->fd, head, length, SEND_TIMEOUT_SECONDS);
    if (sent && !is_head && body_length > 0)
        sent = mr_socket_send_all(conn->fd, marlin_rt_str_data(body), body_length, SEND_TIMEOUT_SECONDS);
    free(head);
    free(request->headers);
    const size_t total = request->total;
    free(request);

    if (!sent || !keep) {
        close_connection(conn);
        return;
    }

    // İsteğin baytları tampondan atılır; arkasından gelenler (pipelining) kalır.
    memmove(conn->data, conn->data + total, conn->length - total);
    conn->length -= total;
    conn->served += 1;
    conn->continue_sent = 0;
    conn->partial = conn->length > 0;
    conn->deadline = mr_monotonic_seconds() + (conn->partial ? REQUEST_TIMEOUT_SECONDS : IDLE_TIMEOUT_SECONDS);

    mr_mutex_lock(&server->returned_lock);
    list_push(&server->returned, conn);
    mr_mutex_unlock(&server->returned_lock);
    // poll'da bekleyen accept'i uyandır (kuyruk doluysa zaten uyanıktır).
    mr_socket_send_all(server->wake[1], "x", 1, 0);
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
    if (!out) marlin_rt_panic("http: bellek yetersiz");
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
