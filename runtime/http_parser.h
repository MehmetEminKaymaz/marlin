// HTTP/1.1 istek başlığı ayrıştırıcısı (RFC 9112).
//
// Yalnızca ayrıştırır: soket okumaz, bellek ayırmaz, global durum tutmaz.
// Sonuçtaki parçalar (HttpSlice) verilen tamponun içini gösterir; tampon
// yaşadıkça geçerlidir. Bu yüzden aynı anda birçok iş parçacığından
// kullanılabilir ve G/Ç biçiminden (engelleyen okuma, epoll ...) bağımsızdır:
// çağıran, elindeki baytlarla tekrar tekrar çağırır; başlık henüz tamamlanmadıysa
// HTTP_PARSE_INCOMPLETE döner.
//
// Desteklenen biçim:
//
//   istek-satırı   = yöntem SP hedef SP "HTTP/1." rakam
//   başlık-satırı  = ad ":" boşluk* değer boşluk*
//   boş satır      → başlık biter; gövdenin uzunluğu Content-Length'tir
//
// Satır sonu CRLF'dir; yalnız LF de kabul edilir (RFC 9112 §2.2). Hedef
// yalnızca "/yol?sorgu" biçiminde olabilir (origin-form). İstek gövdesinde
// Transfer-Encoding (chunked) desteklenmez: 501 ile reddedilir.

#ifndef MARLIN_HTTP_PARSER_H
#define MARLIN_HTTP_PARSER_H

#include <stddef.h>
#include <stdint.h>

#define HTTP_MAX_HEADERS 100

typedef struct {
    const char* data;
    size_t length;
} HttpSlice;

typedef struct {
    HttpSlice method;   // "GET"
    HttpSlice path;     // "/ara"           (hedefin '?' öncesi; çözülmemiş)
    HttpSlice query;    // "q=marlin&s=2"   ('?' sonrası; yoksa boş)
    int minor_version;  // HTTP/1.0 → 0, HTTP/1.1 → 1

    HttpSlice header_names[HTTP_MAX_HEADERS];
    HttpSlice header_values[HTTP_MAX_HEADERS];  // baştaki ve sondaki boşluklar atılmış
    size_t header_count;

    size_t head_length;      // boş satır dahil başlığın bayt sayısı; gövde buradan başlar
    int64_t content_length;  // Content-Length; yoksa 0
} HttpHead;

typedef enum {
    HTTP_PARSE_OK,          // başlık tamam, `out` dolu
    HTTP_PARSE_INCOMPLETE,  // boş satıra henüz gelinmedi; daha çok bayt gerekiyor
    HTTP_PARSE_ERROR        // geçersiz istek; `*status` gönderilecek hata kodu
} HttpParseResult;

// `data`nın başındaki istek başlığını ayrıştırır. Hata durumunda `*status`:
//   400  sözdizimi hatası, Host eksik (HTTP/1.1), çelişen Content-Length
//   431  çok fazla başlık satırı
//   501  Transfer-Encoding
//   505  HTTP/1.x dışında bir sürüm
HttpParseResult http_parse_head(const char* data, size_t length, HttpHead* out, int* status);

// Durum kodunun standart açıklaması ("Not Found"); bilinmiyorsa "".
const char* http_reason_phrase(int status);

// Yanıt başlıklarını doğrulamak için de kullanılan karakter kuralları.
int http_is_token(const char* data, size_t length);        // geçerli bir başlık adı mı?
int http_is_field_value(const char* data, size_t length);  // geçerli bir başlık değeri mi?
int http_equals_ignore_case(HttpSlice s, const char* text);

#endif // MARLIN_HTTP_PARSER_H
