// minik çalışma zamanı kütüphanesi (libminikrt.a)
//
// Derlenen her minik programına bağlanır. Dilin MLIR'de karşılığı olmayan
// işlemleri (çıktı ve çalışma zamanı hata mesajları) burada, C ile yazılır. Derleyici bu
// fonksiyonlara çağrı üretir (bkz. MLIRBackend/MLIRBuiltinManager.cpp);
// imzalar iki tarafta aynı olmalıdır.
//
// Argümanlar bilerek en az 32 bit genişliğindedir: daha dar tamsayıların
// çağrı sırasında nasıl genişletileceği platforma bağlıdır.

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
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

void minik_print_str(const char* data, int64_t length) {
    fwrite(data, 1, (size_t)length, stdout);
}

// Kayan nokta değerini, geri okunduğunda aynı değeri veren EN KISA ondalık
// gösterimle yazar (0.1 → "0.1", 1/3 → "0.3333333333333333"). Tam sayı
// değerli sonuçlara ".0" eklenir ki çıktıda tamsayıdan ayırt edilebilsin.
static void print_shortest(double value, int isSingle) {
    char buffer[40];
    const int maxDigits = isSingle ? 9 : 17;
    for (int digits = 1; digits <= maxDigits; ++digits) {
        snprintf(buffer, sizeof buffer, "%.*g", digits, value);
        if (isSingle ? strtof(buffer, NULL) == (float)value
                     : strtod(buffer, NULL) == value)
            break;
    }
    // %g, basamak sayısı azsa büyük sayıları üslü yazar (4000 → "4e+03").
    // 10^16'ya kadar olan sayılar açık yazılır; üslü gösterim yalnızca çok
    // büyük ve çok küçük sayılara kalır.
    const char* exponent = strchr(buffer, 'e');
    if (exponent && atoi(exponent + 1) > 0 && atoi(exponent + 1) < 16)
        snprintf(buffer, sizeof buffer, "%.0f", value);

    fputs(buffer, stdout);
    if (!strpbrk(buffer, ".einf")) fputs(".0", stdout); // inf / nan'a dokunma
}

void minik_print_f64(double value) { print_shortest(value, 0); }

void minik_print_f32(float value) { print_shortest(value, 1); }
