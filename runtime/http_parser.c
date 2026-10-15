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

    int host_count = 0, has_transfer_encoding = 0, has_content_length = 0;
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
            has_transfer_encoding = 1;
        } else if (http_equals_ignore_case(name, "host")) {
            ++host_count;
        }
    }

    // Parçalı (chunked) gövde desteklenmiyor.
    if (has_transfer_encoding) return fail(status, 501);
    // HTTP/1.1 isteğinde tam olarak bir Host olmalı (RFC 9112 §3.2).
    if (host_count > 1 || (out->minor_version >= 1 && host_count == 0)) return fail(status, 400);

    out->head_length = pos;
    return HTTP_PARSE_OK;
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
