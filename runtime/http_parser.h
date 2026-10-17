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
// yalnızca "/yol?sorgu" biçiminde olabilir (origin-form).
//
// GÖVDENİN UZUNLUĞU (RFC 9112 §6)
//
//   * Content-Length: gövde o kadar bayttır.
//   * Transfer-Encoding: chunked: gövde parça parça gelir; http_parse_chunked
//     çözer. Son kodlama chunked olmalıdır (değilse 400); chunked'dan başka
//     bir kodlama (gzip ...) desteklenmez (501).
//   * İkisi birden: 400. Aynı istek için iki farklı uzunluk, araya giren bir
//     vekille sunucunun istekleri farklı bölmesine (request smuggling) yol
//     açar; reddedilir.
//   * HTTP/1.0'da Transfer-Encoding: 400 (o sürümde tanımlı değildir).

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
    int chunked;             // gövde parçalı mı (Transfer-Encoding: chunked)?
} HttpHead;

typedef enum {
    HTTP_PARSE_OK,          // başlık tamam, `out` dolu
    HTTP_PARSE_INCOMPLETE,  // boş satıra henüz gelinmedi; daha çok bayt gerekiyor
    HTTP_PARSE_ERROR        // geçersiz istek; `*status` gönderilecek hata kodu
} HttpParseResult;

// `data`nın başındaki istek başlığını ayrıştırır. Hata durumunda `*status`:
//   400  sözdizimi hatası, Host eksik (HTTP/1.1), çelişen Content-Length,
//        Content-Length ile Transfer-Encoding birlikte, son kodlaması chunked
//        olmayan Transfer-Encoding
//   431  çok fazla başlık satırı
//   501  chunked dışında bir aktarım kodlaması (gzip ...)
//   505  HTTP/1.x dışında bir sürüm
HttpParseResult http_parse_head(const char* data, size_t length, HttpHead* out, int* status);

// Parçalı gövde (RFC 9112 §7.1):
//
//   parça-boyu [; uzantı] CRLF  veri CRLF  ...  0 CRLF  [sondaki-başlık CRLF]*  CRLF
//
// `data` gövdenin başını (başlıktan sonrasını) gösterir. OK'de `*body_length`
// çözülmüş gövdenin, `*consumed` gövdenin ham bayt sayısıdır (sonraki istek
// buradan başlar). `out` NULL ise yalnızca ölçer; değilse çözülmüş gövde
// oraya yazılır (en az *body_length bayt yer olmalı: önce ölçülür).
// Uzantılar ve sondaki başlıklar okunup atılır. Hata durumunda `*status`:
//   400  sözdizimi hatası;  413  gövde `max_body`den büyük
HttpParseResult http_parse_chunked(const char* data, size_t length, size_t max_body, char* out,
                                   size_t* body_length, size_t* consumed, int* status);

// Durum kodunun standart açıklaması ("Not Found"); bilinmiyorsa "".
const char* http_reason_phrase(int status);

// Yanıt başlıklarını doğrulamak için de kullanılan karakter kuralları.
int http_is_token(const char* data, size_t length);        // geçerli bir başlık adı mı?
int http_is_field_value(const char* data, size_t length);  // geçerli bir başlık değeri mi?
int http_equals_ignore_case(HttpSlice s, const char* text);

#endif // MARLIN_HTTP_PARSER_H
