// Çalışma zamanı kütüphanesinin kendi dosyaları arasında paylaşılan yardımcılar.
//
// Derlenen programlar bu başlığı görmez; yalnızca runtime/ altındaki C
// dosyaları (marlin_rt.c ve standart kütüphanenin std_*.c dosyaları) kullanır.
//
// Standart kütüphane fonksiyonlarının adları ve imzaları std/*.marl
// dosyalarındaki `extern func` bildirimleriyle eşleşmelidir:
//
//   std/file.marl:   extern func c_exists(string path) -> bool;
//   std_file.c:       int32_t marlin_file_exists(int64_t path)
//
//   marlin tipi          C tipi
//   int / int64         int32_t / int64_t
//   float / float64     float / double
//   bool, char          int32_t  (dar tamsayılar 32 bit olarak geçer)
//   string              int64_t  (string bloğunun adresi; aşağıdaki
//                                 yardımcılarla okunur ve oluşturulur)

#ifndef MARLIN_RT_H
#define MARLIN_RT_H

#include <stdint.h>
#include <stdio.h>

// --- string ---
// Stringler değişmez: bir fonksiyon aldığı stringi değiştirmez, sonucunu her
// zaman yeni bir string olarak döndürür.

int64_t marlin_str_len(int64_t s);
// Stringin baytları; sonunda \0 vardır (C fonksiyonlarına doğrudan verilebilir).
const char* marlin_rt_str_data(int64_t s);
// Verilen baytların kopyasından yeni bir string.
int64_t marlin_rt_str_new(const char* data, int64_t length);
int64_t marlin_rt_str_from_text(const char* text);
// `length` baytlık yeni bir string ayırır; baytlarını çağıran doldurur.
char* marlin_rt_str_alloc(int64_t length, int64_t* string);

// --- hata ---
// "calisma zamani hatasi: <mesaj>" yazar ve programı 1 koduyla bitirir.
void marlin_rt_panic(const char* format, ...);

// --- satır okuma (std.file ve std.io ortak kullanır) ---
// Bir satır okur; satır sonu (\n ya da \r\n) sonuca dahil edilmez. Okunacak
// bir şey kalmadıysa boş string döner.
int64_t marlin_rt_read_line(FILE* stream);
// Okunacak veri kalmadı mı? (Bir sonraki okuma boş dönecekse doğru.)
int32_t marlin_rt_at_eof(FILE* stream);

// --- dış C kütüphanesini çalışırken yükleme (marlin_library.c) ---
// std.postgres (libpq) ve std.http.client (libcurl) bu kütüphanelere
// bağlanmaz: program kütüphanesi olmayan makinede de açılır, yalnızca modülü
// kullanınca açıklayıcı bir hata alır.

typedef struct {
    const char* name;  // sembolün adı; dizinin sonu NULL
    void* slot;        // adresin yazılacağı fonksiyon işaretçisinin adresi
} MarlinSymbol;

typedef struct {
    int state;         // 0 denenmedi, 1 deneniyor, 2 yüklendi, 3 yüklenemedi
    char error[512];
} MarlinLibrary;

// İlk çağrıda kütüphaneyi yükleyip sembolleri doldurur (aynı anda çağıran iş
// parçacıkları sonucu bekler); sonraki çağrılar yalnızca sonucu verir.
// Başarılıysa 1; değilse 0 ve sebep lib->error'dadır. Aranan yerler: `env`
// ortam değişkenindeki dosya yolu (verilmişse yalnızca o), sonra sırayla
// `candidates` (NULL ile biter). `hint`: bulunamazsa önerilen kurulum.
int marlin_rt_load_library(MarlinLibrary* lib, const char* what, const char* env,
                           const char* const* candidates, const MarlinSymbol* symbols, const char* hint);

#endif // MARLIN_RT_H
