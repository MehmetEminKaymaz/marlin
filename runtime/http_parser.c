// HTTP/1.1 istek başlığı ayrıştırıcısı (bkz. http_parser.h).
//
// Kurallar RFC 9112'den (HTTP/1.1 mesaj biçimi) ve RFC 9110'dan (alan
// adlarının karakterleri) alınmıştır; her kontrolün yanında ilgili madde yazar.

#include "http_parser.h"

#include <string.h>

// Belirteç (token) karakteri: yöntem ve başlık adları bunlardan oluşur
// (RFC 9110 §5.6.2).
static int is_tchar(unsigned char c) {
    if (c >= '0' && c <= '9') return 1;
    if ((c | 0x20) >= 'a' && (c | 0x20) <= 'z') return 1;
    return c != 0 && strchr("!#$%&'*+-.^_`|~", c) != NULL;
}

// Başlık değerinde izin verilen karakter: görünür karakterler, boşluk, sekme
// ve 0x80 üstü baytlar (RFC 9110 §5.5). CR, LF, NUL ve diğer denetim
// karakterleri yasaktır.
static int is_field_char(unsigned char c) {
    return c == '\t' || (c >= 0x20 && c != 0x7F);
}

int http_equals_ignore_case(HttpSlice s, const char* text) {
    const size_t n = strlen(text);
    if (s.length != n) return 0;
    for (size_t i = 0; i < n; ++i)
        if ((s.data[i] | 0x20) != (text[i] | 0x20)) return 0;
    return 1;
}

int http_is_token(const char* data, size_t length) {
    if (length == 0) return 0;
    for (size_t i = 0; i < length; ++i)
        if (!is_tchar((unsigned char)data[i])) return 0;
    return 1;
}

int http_is_field_value(const char* data, size_t length) {
    for (size_t i = 0; i < length; ++i)
        if (!is_field_char((unsigned char)data[i])) return 0;
    return 1;
}

// `pos`tan başlayan satırı bulur. Satır sonu LF'dir; önündeki tek CR satıra
// dahil edilmez. LF henüz gelmediyse 0 döner (satır eksik).
static int next_line(const char* data, size_t length, size_t* pos, HttpSlice* line) {
    const char* start = data + *pos;
    const char* lf = memchr(start, '\n', length - *pos);
    if (!lf) return 0;
    size_t n = (size_t)(lf - start);
    if (n > 0 && start[n - 1] == '\r') --n;
    line->data = start;
    line->length = n;
    *pos = (size_t)(lf - data) + 1;
    return 1;
}

static HttpParseResult fail(int* status, int code) {
    *status = code;
    return HTTP_PARSE_ERROR;
}

// istek-satırı = yöntem SP hedef SP sürüm   (RFC 9112 §3)
static HttpParseResult parse_request_line(HttpSlice line, HttpHead* out, int* status) {
    size_t i = 0;
    while (i < line.length && is_tchar((unsigned char)line.data[i])) ++i;
    if (i == 0 || i >= line.length || line.data[i] != ' ') return fail(status, 400);
    out->method = (HttpSlice){line.data, i};

    const size_t target_start = ++i;
    while (i < line.length && line.data[i] > 0x20 && line.data[i] < 0x7F) ++i;
    if (i == target_start || i >= line.length || line.data[i] != ' ') return fail(status, 400);
    HttpSlice target = {line.data + target_start, i - target_start};

    // Yalnızca origin-form: "/yol" ya da "/yol?sorgu" (RFC 9112 §3.2.1).
    if (target.data[0] != '/') return fail(status, 400);
    const char* question = memchr(target.data, '?', target.length);
    if (question) {
        out->path = (HttpSlice){target.data, (size_t)(question - target.data)};
        out->query = (HttpSlice){question + 1, target.length - out->path.length - 1};
    } else {
        out->path = target;
        out->query = (HttpSlice){target.data + target.length, 0};
    }

    // sürüm = "HTTP/" rakam "." rakam   (RFC 9112 §2.3)
    HttpSlice version = {line.data + i + 1, line.length - i - 1};
    if (version.length != 8 || memcmp(version.data, "HTTP/", 5) != 0 ||
        version.data[5] < '0' || version.data[5] > '9' || version.data[6] != '.' ||
        version.data[7] < '0' || version.data[7] > '9')
        return fail(status, 400);
    if (version.data[5] != '1') return fail(status, 505);
    out->minor_version = version.data[7] - '0';
    return HTTP_PARSE_OK;
}

// Transfer-Encoding = #transfer-coding  (RFC 9112 §6.1): virgülle ayrılmış
// kodlamalar, uygulanma sırasıyla. Birden çok başlık satırı birleşik sayılır.
typedef struct {
    int present;       // başlık var mı?
    int last_chunked;  // son kodlama chunked mı?
    int chunked_count; // chunked kaç kez geçti?
    int other;         // chunked dışında bir kodlama var mı?
} TransferCodings;

static void note_transfer_codings(HttpSlice value, TransferCodings* t) {
    t->present = 1;
    size_t start = 0;
    while (start <= value.length) {
        size_t end = start;
        while (end < value.length && value.data[end] != ',') ++end;
        size_t a = start, b = end;
        while (a < b && (value.data[a] == ' ' || value.data[a] == '\t')) ++a;
        while (b > a && (value.data[b - 1] == ' ' || value.data[b - 1] == '\t')) --b;
        if (b > a) { // (boş liste elemanları yok sayılır: RFC 9110 §5.6.1)
            HttpSlice coding = {value.data + a, b - a};
            t->last_chunked = http_equals_ignore_case(coding, "chunked");
            if (t->last_chunked) ++t->chunked_count;
            else t->other = 1;
        }
        start = end + 1;
    }
}

// Content-Length = 1*rakam   (RFC 9112 §6.3; RFC 9110 §8.6)
static int parse_content_length(HttpSlice value, int64_t* result) {
    if (value.length == 0 || value.length > 15) return 0; // 15 hane: ~1 PB, taşmaz
    int64_t n = 0;
    for (size_t i = 0; i < value.length; ++i) {
        if (value.data[i] < '0' || value.data[i] > '9') return 0;
        n = n * 10 + (value.data[i] - '0');
    }
    *result = n;
    return 1;
}

HttpParseResult http_parse_head(const char* data, size_t length, HttpHead* out, int* status) {
    memset(out, 0, sizeof *out);
    size_t pos = 0;
    HttpSlice line;

    // İstek satırından önceki boş satırlar yok sayılır (RFC 9112 §2.2).
    do {
        if (!next_line(data, length, &pos, &line)) return HTTP_PARSE_INCOMPLETE;
    } while (line.length == 0);

    HttpParseResult result = parse_request_line(line, out, status);
    if (result != HTTP_PARSE_OK) return result;

    int host_count = 0, has_content_length = 0;
    TransferCodings codings = {0, 0, 0, 0};
    for (;;) {
        if (!next_line(data, length, &pos, &line)) return HTTP_PARSE_INCOMPLETE;
        if (line.length == 0) break; // boş satır: başlık bitti

        // Boşlukla başlayan satır, eski "satır katlama" biçimidir (RFC 9112 §5.2).
        if (line.data[0] == ' ' || line.data[0] == '\t') return fail(status, 400);

        // ad ":" — adla iki nokta arasında boşluk olamaz (RFC 9112 §5.1).
        size_t i = 0;
        while (i < line.length && is_tchar((unsigned char)line.data[i])) ++i;
        if (i == 0 || i >= line.length || line.data[i] != ':') return fail(status, 400);
        HttpSlice name = {line.data, i};

        // değer: baştaki ve sondaki boşluklar değere dahil değildir.
        size_t begin = i + 1, end = line.length;
        while (begin < end && (line.data[begin] == ' ' || line.data[begin] == '\t')) ++begin;
        while (end > begin && (line.data[end - 1] == ' ' || line.data[end - 1] == '\t')) --end;
        for (size_t k = begin; k < end; ++k)
            if (!is_field_char((unsigned char)line.data[k])) return fail(status, 400);
        HttpSlice value = {line.data + begin, end - begin};

        if (out->header_count == HTTP_MAX_HEADERS) return fail(status, 431);
        out->header_names[out->header_count] = name;
        out->header_values[out->header_count] = value;
        ++out->header_count;

        if (http_equals_ignore_case(name, "content-length")) {
            // Birden çok Content-Length ancak hepsi aynıysa kabul edilir.
            int64_t n;
            if (!parse_content_length(value, &n)) return fail(status, 400);
            if (has_content_length && n != out->content_length) return fail(status, 400);
            out->content_length = n;
            has_content_length = 1;
        } else if (http_equals_ignore_case(name, "transfer-encoding")) {
            note_transfer_codings(value, &codings);
        } else if (http_equals_ignore_case(name, "host")) {
            ++host_count;
        }
    }

    // Gövdenin uzunluğu (RFC 9112 §6.1, §6.3): bkz. http_parser.h
    if (codings.present) {
        if (out->minor_version == 0) return fail(status, 400);   // HTTP/1.0'da tanımlı değil
        if (has_content_length) return fail(status, 400);        // iki uzunluk: request smuggling
        if (!codings.last_chunked || codings.chunked_count > 1) return fail(status, 400);
        if (codings.other) return fail(status, 501);             // gzip, chunked ...
        out->chunked = 1;
        out->content_length = 0;
    }
    // HTTP/1.1 isteğinde tam olarak bir Host olmalı (RFC 9112 §3.2).
    if (host_count > 1 || (out->minor_version >= 1 && host_count == 0)) return fail(status, 400);

    out->head_length = pos;
    return HTTP_PARSE_OK;
}

// ---------------------------------------------------------------------------
// Parçalı gövde (RFC 9112 §7.1)
// ---------------------------------------------------------------------------

enum { MAX_CHUNK_LINE = 4096, MAX_TRAILER_BYTES = 64 * 1024 };

static int hex_digit(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if ((c | 0x20) >= 'a' && (c | 0x20) <= 'f') return (c | 0x20) - 'a' + 10;
    return -1;
}

// `pos`tan başlayan satırın sonu (LF); yoksa satır tamamlanmamıştır. Çok uzun
// satır (bitmeyen parça başlığı) hata olsun diye `limit` aşılınca -2.
static long find_line_end(const char* data, size_t length, size_t pos, size_t limit) {
    const char* lf = memchr(data + pos, '\n', length - pos);
    if (!lf) return length - pos > limit ? -2 : -1;
    if ((size_t)(lf - data) - pos > limit) return -2;
    return (long)(lf - data);
}

HttpParseResult http_parse_chunked(const char* data, size_t length, size_t max_body, char* out,
                                   size_t* body_length, size_t* consumed, int* status) {
    size_t pos = 0, body = 0;
    for (;;) {
        // parça-boyu [; uzantı] CRLF
        const long lf = find_line_end(data, length, pos, MAX_CHUNK_LINE);
        if (lf == -1) return HTTP_PARSE_INCOMPLETE;
        if (lf == -2) return fail(status, 400);
        size_t end = (size_t)lf;
        if (end > pos && data[end - 1] == '\r') --end;

        size_t i = pos, size = 0;
        if (i == end || hex_digit(data[i]) < 0) return fail(status, 400);
        for (; i < end && hex_digit(data[i]) >= 0; ++i) {
            if (size > (SIZE_MAX - 15) / 16) return fail(status, 413);
            size = size * 16 + (size_t)hex_digit(data[i]);
        }
        // Uzantılar (;ad=değer) okunup atılır; geri kalanında denetim karakteri olamaz.
        while (i < end && (data[i] == ' ' || data[i] == '\t')) ++i;
        if (i < end && data[i] != ';') return fail(status, 400);
        for (; i < end; ++i)
            if ((unsigned char)data[i] < 0x20 && data[i] != '\t') return fail(status, 400);
        pos = (size_t)lf + 1;

        // Son parça (0): ardından sondaki başlıklar ve boş satır.
        if (size == 0) {
            const size_t trailerStart = pos;
            for (;;) {
                const long tl = find_line_end(data, length, pos, MAX_TRAILER_BYTES);
                if (tl == -1) return HTTP_PARSE_INCOMPLETE;
                if (tl == -2 || (size_t)tl - trailerStart > MAX_TRAILER_BYTES) return fail(status, 400);
                size_t te = (size_t)tl;
                if (te > pos && data[te - 1] == '\r') --te;
                if (te == pos) { // boş satır: gövde bitti
                    *body_length = body;
                    *consumed = (size_t)tl + 1;
                    return HTTP_PARSE_OK;
                }
                // ad ":" değer — adın geçerli olması yeter; değer atılır.
                size_t k = pos;
                while (k < te && data[k] != ':') ++k;
                if (k == pos || k == te || !http_is_token(data + pos, k - pos)) return fail(status, 400);
                pos = (size_t)tl + 1;
            }
        }

        // veri CRLF
        if (size > max_body - body) return fail(status, 413);
        if (length - pos < size + 1) return HTTP_PARSE_INCOMPLETE;
        if (out) memcpy(out + body, data + pos, size);
        body += size;
        pos += size;
        if (data[pos] == '\r') {
            if (length - pos < 2) return HTTP_PARSE_INCOMPLETE;
            ++pos;
        }
        if (data[pos] != '\n') return fail(status, 400);
        ++pos;
    }
}

const char* http_reason_phrase(int status) {
    switch (status) {
        case 100: return "Continue";
        case 200: return "OK";
        case 201: return "Created";
        case 202: return "Accepted";
        case 204: return "No Content";
        case 301: return "Moved Permanently";
        case 302: return "Found";
        case 303: return "See Other";
        case 304: return "Not Modified";
        case 307: return "Temporary Redirect";
        case 308: return "Permanent Redirect";
        case 400: return "Bad Request";
        case 401: return "Unauthorized";
        case 403: return "Forbidden";
        case 404: return "Not Found";
        case 405: return "Method Not Allowed";
        case 408: return "Request Timeout";
        case 409: return "Conflict";
        case 411: return "Length Required";
        case 413: return "Content Too Large";
        case 415: return "Unsupported Media Type";
        case 422: return "Unprocessable Content";
        case 429: return "Too Many Requests";
        case 431: return "Request Header Fields Too Large";
        case 500: return "Internal Server Error";
        case 501: return "Not Implemented";
        case 502: return "Bad Gateway";
        case 503: return "Service Unavailable";
        case 505: return "HTTP Version Not Supported";
        default:  return "";
    }
}
