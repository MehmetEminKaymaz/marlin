// Çalışma zamanı kütüphanesinin kendi dosyaları arasında paylaşılan yardımcılar.
//
// Derlenen programlar bu başlığı görmez; yalnızca runtime/ altındaki C
// dosyaları (minik_rt.c ve standart kütüphanenin std_*.c dosyaları) kullanır.
//
// Standart kütüphane fonksiyonlarının adları ve imzaları std/*.minik
// dosyalarındaki `extern func` bildirimleriyle eşleşmelidir:
//
//   std/file.minik:   extern func c_exists(string path) -> bool;
//   std_file.c:       int32_t minik_file_exists(int64_t path)
//
//   minik tipi          C tipi
//   int / int64         int32_t / int64_t
//   float / float64     float / double
//   bool, char          int32_t  (dar tamsayılar 32 bit olarak geçer)
//   string              int64_t  (string bloğunun adresi; aşağıdaki
//                                 yardımcılarla okunur ve oluşturulur)

#ifndef MINIK_RT_H
#define MINIK_RT_H

#include <stdint.h>
#include <stdio.h>

// --- string ---
// Stringler değişmez: bir fonksiyon aldığı stringi değiştirmez, sonucunu her
// zaman yeni bir string olarak döndürür.

int64_t minik_str_len(int64_t s);
// Stringin baytları; sonunda \0 vardır (C fonksiyonlarına doğrudan verilebilir).
const char* minik_rt_str_data(int64_t s);
// Verilen baytların kopyasından yeni bir string.
int64_t minik_rt_str_new(const char* data, int64_t length);
int64_t minik_rt_str_from_text(const char* text);
// `length` baytlık yeni bir string ayırır; baytlarını çağıran doldurur.
char* minik_rt_str_alloc(int64_t length, int64_t* string);

// --- hata ---
// "calisma zamani hatasi: <mesaj>" yazar ve programı 1 koduyla bitirir.
void minik_rt_panic(const char* format, ...);

// --- satır okuma (std.file ve std.io ortak kullanır) ---
// Bir satır okur; satır sonu (\n ya da \r\n) sonuca dahil edilmez. Okunacak
// bir şey kalmadıysa boş string döner.
int64_t minik_rt_read_line(FILE* stream);
// Okunacak veri kalmadı mı? (Bir sonraki okuma boş dönecekse doğru.)
int32_t minik_rt_at_eof(FILE* stream);

#endif // MINIK_RT_H
