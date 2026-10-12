// minik çalışma zamanı kütüphanesi (libminikrt.a)
//
// Derlenen her minik programına bağlanır. Dilin MLIR'de karşılığı olmayan
// işlemleri (çıktı, stringler ve çalışma zamanı hata mesajları) burada, C ile yazılır. Derleyici bu
// fonksiyonlara çağrı üretir (bkz. MLIRBackend/MLIRBuiltinManager.cpp);
// imzalar iki tarafta aynı olmalıdır.
//
// Argümanlar bilerek en az 32 bit genişliğindedir: daha dar tamsayıların
// çağrı sırasında nasıl genişletileceği platforma bağlıdır.

#define _GNU_SOURCE // getline, flockfile

#include "minik_rt.h"

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

// --- Çalışma zamanı hataları ---
// Program devam edemeyecek bir hata yaptığında (sınır dışı indeks gibi) mesaj
// yazıp 1 koduyla çıkar. Önce stdout boşaltılır ki o ana kadarki çıktı kaybolmasın.

// Çıkış _exit iledir (exit değil): exit, program kapanırken çalışan temizlik
// kodlarını çağırır; paralel döngülerin iş parçacığı havuzu orada bütün iş
// parçacıklarının bitmesini bekler ve hata yapan iş parçacığı hiç bitmeyeceği
// için program kilitlenebilir.
static void fail(void) {
    fflush(stdout);
    fflush(stderr);
    _exit(1);
}

// Her hata fonksiyonunun ilk işi. Paralel döngüde birden çok iş parçacığı aynı
// anda hata yapabilir: stderr kilitlenir ve bir daha bırakılmaz, böylece
// yalnızca ilk hatanın mesajı yazılır (diğerleri program çıkana kadar bekler).
static void begin_panic(void) {
    flockfile(stderr);
    fflush(stdout);
}

void minik_panic_index(int64_t index, int64_t length, int32_t line) {
    begin_panic();
    fprintf(stderr, "calisma zamani hatasi (satir %d): liste indeksi sinir disinda (indeks %lld, uzunluk %lld)\n",
            (int)line, (long long)index, (long long)length);
    fail();
}

void minik_panic_pop(int32_t line) {
    begin_panic();
    fprintf(stderr, "calisma zamani hatasi (satir %d): bos listeden pop yapilamaz\n", (int)line);
    fail();
}

// Matris hata mesajlarındaki sayılar (şekiller, indeksler) boyut sayısına göre
// değişir. Derleyici onları hata fonksiyonundan hemen önce minik_report_push
// ile tek tek bildirir; iki ayrı liste tutulur (slot 0 ve 1). Listeler iş
// parçacığına özeldir: paralel döngüde iki iş parçacığının sayıları karışmaz.
#define REPORT_MAX 16
static _Thread_local long long report_values[2][REPORT_MAX];
static _Thread_local int report_count[2];

void minik_report_push(int64_t slot, int64_t value) {
    int s = slot != 0;
    if (report_count[s] < REPORT_MAX) report_values[s][report_count[s]++] = (long long)value;
}

// Listeyi verilen ayraçla yazar: "2x3x4" ya da "1, 2".
static void report_print(int slot, const char* separator) {
    for (int i = 0; i < report_count[slot]; ++i)
        fprintf(stderr, "%s%lld", i ? separator : "", report_values[slot][i]);
}

// slot 0: indeksler, slot 1: boyut uzunlukları
void minik_panic_matrix_index(int32_t line) {
    begin_panic();
    fprintf(stderr, "calisma zamani hatasi (satir %d): matris indeksi sinir disinda (indeks [", (int)line);
    report_print(0, ", ");
    fputs("], boyut ", stderr);
    report_print(1, "x");
    fputs(")\n", stderr);
    fail();
}

// slot 0 ve 1: iki matrisin şekilleri
void minik_panic_shape(int32_t line) {
    begin_panic();
    fprintf(stderr, "calisma zamani hatasi (satir %d): matris boyutlari uyusmuyor (", (int)line);
    report_print(0, "x");
    fputs(" ve ", stderr);
    report_print(1, "x");
    fputs(")\n", stderr);
    fail();
}

void minik_panic_matmul(int32_t line) {
    begin_panic();
    fprintf(stderr, "calisma zamani hatasi (satir %d): matmul icin boyutlar uygun degil (", (int)line);
    report_print(0, "x");
    fputs(" ve ", stderr);
    report_print(1, "x");
    fputs("); soldakinin sutun sayisi sagdakinin satir sayisina esit olmali\n", stderr);
    fail();
}

// --- time ---
// Saniye cinsinden zaman. Başlangıç anı belirsizdir (makinenin açılışı);
// yalnızca iki çağrının farkı anlamlıdır. Saat geri gitmez (sistem saati
// değiştirilse bile), bu yüzden süre ölçmeye uygundur.
double minik_time(void) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (double)now.tv_sec + (double)now.tv_nsec / 1e9;
}

// --- print ---
//
// Bir print çağrısı birden çok parçadan oluşur (değerler, boşluklar, satır
// sonu). Paralel döngüde iki iş parçacığının parçaları birbirine girmesin diye
// derleyici her print'i minik_print_begin / minik_print_end arasına alır;
// stdout bu arada çağıran iş parçacığına kilitlidir. Böylece satırların sırası
// belirsiz olsa da her satır bütün çıkar.

void minik_print_begin(void) { flockfile(stdout); }

void minik_print_end(void) { funlockfile(stdout); }

void minik_print_i64(int64_t value) { printf("%lld", (long long)value); }

void minik_print_u64(uint64_t value) { printf("%llu", (unsigned long long)value); }

void minik_print_bool(int32_t value) { fputs(value ? "true" : "false", stdout); }

void minik_print_char(int32_t value) { putchar(value); }

// Kayan nokta değerini, geri okunduğunda aynı değeri veren EN KISA ondalık
// gösterimle `buffer`a yazar (0.1 → "0.1", 1/3 → "0.3333333333333333"). Tam
// sayı değerli sonuçlara ".0" eklenir ki tamsayıdan ayırt edilebilsin.
#define FLOAT_TEXT_MAX 48
static void format_shortest(char* buffer, double value, int isSingle) {
    const int maxDigits = isSingle ? 9 : 17;
    for (int digits = 1; digits <= maxDigits; ++digits) {
        snprintf(buffer, 40, "%.*g", digits, value);
        if (isSingle ? strtof(buffer, NULL) == (float)value
                     : strtod(buffer, NULL) == value)
            break;
    }
    // %g, basamak sayısı azsa büyük sayıları üslü yazar (4000 → "4e+03").
    // 10^16'ya kadar olan sayılar açık yazılır; üslü gösterim yalnızca çok
    // büyük ve çok küçük sayılara kalır.
    const char* exponent = strchr(buffer, 'e');
    if (exponent && atoi(exponent + 1) > 0 && atoi(exponent + 1) < 16)
        snprintf(buffer, 40, "%.0f", value);

    if (!strpbrk(buffer, ".einf")) strcat(buffer, ".0"); // inf / nan'a dokunma
}

void minik_print_f64(double value) {
    char buffer[FLOAT_TEXT_MAX];
    format_shortest(buffer, value, 0);
    fputs(buffer, stdout);
}

void minik_print_f32(float value) {
    char buffer[FLOAT_TEXT_MAX];
    format_shortest(buffer, value, 1);
    fputs(buffer, stdout);
}

// --- string ---
//
// String, başında bir başlık bulunan bellek bloğudur; değeri bu bloğun
// adresidir (derleyici onu 64 bitlik bir tamsayı olarak taşır):
//
//   [ refs | length | baytlar ... | \0 ]
//
//   refs    -1: kaynak koddaki bir literal (programın sabit verisidir, asla
//               bırakılmaz); 1: çalışırken oluşturulmuş. İleride başvuru
//               sayımına geçilirse sayaç burada tutulacak.
//   length  bayt sayısı (sondaki \0 hariç). Metin UTF-8'dir; uzunluk ve
//           indeksler bayt sayar.
//
// Derleyici literalleri aynı düzende üretir (bkz. MLIRGeneratorExpr.cpp,
// StringLiteral); iki taraf uyumlu olmalıdır.
//
// Stringler DEĞİŞMEZ: her işlem yeni bir string üretir, var olanı değiştirmez.
// Bu yüzden bir stringi kopyalamak adresini kopyalamaktır. Bellekleri
// kendiliğinden bırakılmaz; `free(s)` ile bırakılır (listelerdeki gibi).
//
// 0 adresi boş string sayılır: başlangıç değeri verilmemiş değişkenler ve
// sıfırlanmış dizi elemanları böyledir.

typedef struct {
    int64_t refs;
    int64_t length;
    char data[];
} MinikString;

#define STRING_LITERAL (-1)

static const MinikString* str_of(int64_t handle) { return (const MinikString*)(intptr_t)handle; }
static int64_t str_length(int64_t handle) { return handle ? str_of(handle)->length : 0; }
static const char* str_data(int64_t handle) { return handle ? str_of(handle)->data : ""; }

// `length` baytlık yeni bir string ayırır; içeriğini çağıran doldurur.
static MinikString* str_alloc(int64_t length) {
    MinikString* s = malloc(sizeof(MinikString) + (size_t)length + 1);
    if (!s) {
        begin_panic();
        fputs("calisma zamani hatasi: string icin bellek ayrilamadi\n", stderr);
        fail();
    }
    s->refs = 1;
    s->length = length;
    s->data[length] = '\0';
    return s;
}

static int64_t str_from_bytes(const char* data, int64_t length) {
    MinikString* s = str_alloc(length);
    memcpy(s->data, data, (size_t)length);
    return (int64_t)(intptr_t)s;
}

static int64_t str_from_text(const char* text) { return str_from_bytes(text, (int64_t)strlen(text)); }

void minik_print_str(int64_t s) { fwrite(str_data(s), 1, (size_t)str_length(s), stdout); }

int64_t minik_str_len(int64_t s) { return str_length(s); }

// free(s): literaller programın sabit verisidir, dokunulmaz.
void minik_str_free(int64_t s) {
    if (s && str_of(s)->refs != STRING_LITERAL) free((void*)(intptr_t)s);
}

// a + b
int64_t minik_str_concat(int64_t a, int64_t b) {
    const int64_t la = str_length(a), lb = str_length(b);
    MinikString* s = str_alloc(la + lb);
    memcpy(s->data, str_data(a), (size_t)la);
    memcpy(s->data + la, str_data(b), (size_t)lb);
    return (int64_t)(intptr_t)s;
}

// Sözlük sırası (bayt bayt): negatif a < b, sıfır eşit, pozitif a > b.
int32_t minik_str_compare(int64_t a, int64_t b) {
    const int64_t la = str_length(a), lb = str_length(b);
    const int order = memcmp(str_data(a), str_data(b), (size_t)(la < lb ? la : lb));
    if (order != 0) return order < 0 ? -1 : 1;
    return la < lb ? -1 : la > lb;
}

// s[i] — i. bayt
int32_t minik_str_at(int64_t s, int64_t index, int32_t line) {
    if (index < 0 || index >= str_length(s)) {
        begin_panic();
        fprintf(stderr, "calisma zamani hatasi (satir %d): string indeksi sinir disinda (indeks %lld, uzunluk %lld)\n",
                (int)line, (long long)index, (long long)str_length(s));
        fail();
    }
    return (unsigned char)str_data(s)[index];
}

// str(x): değerin print ile yazılan hali
int64_t minik_str_from_i64(int64_t value) {
    char buffer[32];
    snprintf(buffer, sizeof buffer, "%lld", (long long)value);
    return str_from_text(buffer);
}

int64_t minik_str_from_u64(uint64_t value) {
    char buffer[32];
    snprintf(buffer, sizeof buffer, "%llu", (unsigned long long)value);
    return str_from_text(buffer);
}

int64_t minik_str_from_f64(double value) {
    char buffer[FLOAT_TEXT_MAX];
    format_shortest(buffer, value, 0);
    return str_from_text(buffer);
}

int64_t minik_str_from_f32(float value) {
    char buffer[FLOAT_TEXT_MAX];
    format_shortest(buffer, value, 1);
    return str_from_text(buffer);
}

int64_t minik_str_from_bool(int32_t value) { return str_from_text(value ? "true" : "false"); }

int64_t minik_str_from_char(int32_t value) {
    const char c = (char)value;
    return str_from_bytes(&c, 1);
}

// to_int(s) / to_float(s): metnin tamamı bir sayı olmalıdır (baştaki ve
// sondaki boşluklar sayılmaz); değilse program mesajla durur.
static void panic_number(int64_t s, int32_t line, const char* kind) {
    begin_panic();
    fprintf(stderr, "calisma zamani hatasi (satir %d): \"%s\" bir %s degil\n",
            (int)line, str_data(s), kind);
    fail();
}

static int rest_is_blank(const char* text) {
    while (*text == ' ' || *text == '\t' || *text == '\n' || *text == '\r') ++text;
    return *text == '\0';
}

int64_t minik_str_to_i64(int64_t s, int32_t line) {
    const char* text = str_data(s);
    char* end;
    const long long value = strtoll(text, &end, 10);
    if (end == text || !rest_is_blank(end)) panic_number(s, line, "tamsayi");
    return (int64_t)value;
}

double minik_str_to_f64(int64_t s, int32_t line) {
    const char* text = str_data(s);
    char* end;
    const double value = strtod(text, &end);
    if (end == text || !rest_is_blank(end)) panic_number(s, line, "sayi");
    return value;
}

// --- standart kütüphane dosyalarının (std_*.c) kullandığı yardımcılar ---
// (bkz. minik_rt.h)

const char* minik_rt_str_data(int64_t s) { return str_data(s); }

int64_t minik_rt_str_new(const char* data, int64_t length) { return str_from_bytes(data, length); }

int64_t minik_rt_str_from_text(const char* text) { return str_from_text(text); }

char* minik_rt_str_alloc(int64_t length, int64_t* string) {
    MinikString* s = str_alloc(length);
    *string = (int64_t)(intptr_t)s;
    return s->data;
}

void minik_rt_panic(const char* format, ...) {
    begin_panic();
    fputs("calisma zamani hatasi: ", stderr);
    va_list args;
    va_start(args, format);
    vfprintf(stderr, format, args);
    va_end(args);
    fputc('\n', stderr);
    fail();
}

int64_t minik_rt_read_line(FILE* stream) {
    char* line = NULL;
    size_t capacity = 0;
    ssize_t length = getline(&line, &capacity, stream);
    if (length < 0) length = 0; // dosya sonu ya da okuma hatası: boş satır
    if (length > 0 && line[length - 1] == '\n') --length;
    if (length > 0 && line[length - 1] == '\r') --length;
    const int64_t result = str_from_bytes(line ? line : "", (int64_t)length);
    free(line);
    return result;
}

int32_t minik_rt_at_eof(FILE* stream) {
    const int next = fgetc(stream);
    if (next == EOF) return 1;
    ungetc(next, stream);
    return 0;
}
