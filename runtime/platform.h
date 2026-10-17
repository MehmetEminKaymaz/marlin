// Çalışma zamanının işletim sistemine dokunan bütün işleri (bkz. platform_posix.c,
// platform_win32.c).
//
// Runtime'ın geri kalanı yalnızca standart C (C11) ve bu başlığı kullanır;
// <unistd.h>, <pthread.h>, soket ya da <windows.h> başlıklarını doğrudan
// içermez. Yeni bir platform, bu başlıktaki fonksiyonları yazmakla desteklenir.
//
// Desteklenen platformlar: Linux, macOS (POSIX) ve Windows. Runtime GCC ya da
// Clang ile derlenir (Windows'ta clang / clang-cl): __atomic yerleşikleri,
// _Thread_local ve __attribute__((constructor)) bu derleyicilerin hepsinde var.
//
// METİN: Bütün yollar, ortam değişkenleri ve program argümanları UTF-8'dir.
// Windows'ta bunlar sistemin UTF-16 API'lerine çevrilerek kullanılır (ANSI kod
// sayfasına bırakılsa Türkçe harfli dosya adları açılamazdı).

#ifndef MARLIN_PLATFORM_H
#define MARLIN_PLATFORM_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#ifndef _WIN32
#include <pthread.h>
#endif

// ---------------------------------------------------------------------------
// Program
// ---------------------------------------------------------------------------

// Programın argümanları (UTF-8). 0: programın adı.
int mr_arg_count(void);
const char* mr_arg(int index);

// Ortam değişkeninin değeri (UTF-8); tanımlı değilse NULL. Sonuç çağıranındır
// (free ile bırakılır).
char* mr_getenv(const char* name);

// ---------------------------------------------------------------------------
// Standart akışlar
// ---------------------------------------------------------------------------

// Akışı çağıran iş parçacığına kilitler / bırakır (flockfile / _lock_file).
void mr_lock_stream(FILE* stream);
void mr_unlock_stream(FILE* stream);

// ---------------------------------------------------------------------------
// Dosyalar (yollar UTF-8)
// ---------------------------------------------------------------------------

FILE* mr_fopen(const char* path, const char* mode);
int mr_remove(const char* path);       // başarılıysa 1
int mr_file_exists(const char* path);  // 1 / 0

// ---------------------------------------------------------------------------
// Zaman
// ---------------------------------------------------------------------------

// Saniye; başlangıcı belirsiz, geri gitmez (süre ölçmek için).
double mr_monotonic_seconds(void);
// 1 Ocak 1970 UTC'den beri geçen saniye.
double mr_wall_seconds(void);
void mr_sleep_seconds(double seconds);

// ---------------------------------------------------------------------------
// İş parçacıkları: kilit ve koşul değişkeni
// ---------------------------------------------------------------------------

#ifdef _WIN32
typedef struct { void* opaque; } mr_mutex;  // SRWLOCK
typedef struct { void* opaque; } mr_cond;   // CONDITION_VARIABLE
#else
typedef pthread_mutex_t mr_mutex;
typedef pthread_cond_t mr_cond;
#endif

void mr_mutex_init(mr_mutex* m);
void mr_mutex_destroy(mr_mutex* m);
void mr_mutex_lock(mr_mutex* m);
void mr_mutex_unlock(mr_mutex* m);

void mr_cond_init(mr_cond* c);
void mr_cond_destroy(mr_cond* c);
void mr_cond_wait(mr_cond* c, mr_mutex* m);
void mr_cond_signal(mr_cond* c);

// ---------------------------------------------------------------------------
// TCP soketleri
// ---------------------------------------------------------------------------
//
// Soketler engelleyen (blocking) kipte kullanılır. Bir yazma, karşı taraf
// bağlantıyı kapatmış olsa da süreci öldürmez (POSIX'teki SIGPIPE bastırılır).
// Soketler alt süreçlere miras kalmaz.

// POSIX'te dosya tanıtıcısı (int), Windows'ta SOCKET (UINT_PTR); ikisi de sığar.
typedef int64_t mr_socket;
#define MR_NO_SOCKET ((mr_socket)-1)

// mr_socket_recv'in hata sonuçları
#define MR_RECV_ERROR   (-1)
#define MR_RECV_TIMEOUT (-2)

// Bütün ağ arayüzlerinde `port`u dinleyen soket. Başarısızsa MR_NO_SOCKET;
// sebebi `error`a yazılır.
mr_socket mr_tcp_listen(int port, int backlog, char* error, size_t error_size);

// Sıradaki bağlantıyı kabul eder. Başarısızsa MR_NO_SOCKET döner;
// `*transient` doğruysa hata geçicidir (bağlantı kabul edilmeden koptu, tanıtıcı
// ya da bellek bir an için tükendi) ve tekrar denenmelidir.
mr_socket mr_tcp_accept(mr_socket server, int* transient);

// Okuma ve yazma için zaman aşımı (saniye).
void mr_socket_set_timeout(mr_socket s, int seconds);

// Bütün baytları gönderir; karşı taraf kapattıysa ya da hata olduysa 0.
int mr_socket_send_all(mr_socket s, const char* data, size_t length);

// En fazla `capacity` bayt okur: okunan bayt sayısı, karşı taraf kapattıysa 0,
// zaman aşımında MR_RECV_TIMEOUT, başka bir hatada MR_RECV_ERROR.
int64_t mr_socket_recv(mr_socket s, char* buffer, size_t capacity);

// Yazma yönünü kapatıp (karşı taraf yanıtın bittiğini anlar) soketi bırakır.
void mr_socket_close(mr_socket s);

// Bu iş parçacığındaki son soket hatasının açıklaması.
void mr_socket_error_text(char* buffer, size_t size);

#endif // MARLIN_PLATFORM_H
