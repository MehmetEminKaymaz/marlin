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

// Süreci HEMEN bitirir: kapanış kodları (atexit, kütüphanelerin yıkıcıları)
// çalışmaz. Çalışan görevler beklenmez; MLIR'in iş parçacığı havuzunun
// yıkıcısı bütün görevlerin bitmesini bekler ve bitmeyen bir görev (ör. HTTP
// sunucusu) varsa sonsuza dek beklerdi. POSIX'te _Exit; Windows'ta _Exit bile
// DLL'lerin kapanış kodunu çalıştırdığı için TerminateProcess. Açık akışları
// çağıran boşaltmalıdır (fflush).
_Noreturn void mr_exit_now(int code);

// ---------------------------------------------------------------------------
// Paylaşımlı kütüphaneyi çalışırken yükleme (dlopen / LoadLibrary)
// ---------------------------------------------------------------------------

// Kütüphaneyi yükler; bulunamazsa NULL (programı durdurmaz). Ad, sistemin
// arama yollarında aranır ("libcuda.so.1", "nvcuda.dll").
void* mr_library_open(const char* name);
// Kütüphanedeki sembolün adresi; yoksa NULL.
void* mr_library_symbol(void* library, const char* name);

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
#define MR_RECV_ERROR       (-1)
#define MR_RECV_TIMEOUT     (-2)  // engelleyen kipte zaman aşımı
#define MR_RECV_WOULD_BLOCK (-3)  // engellemeyen kipte: şu an okunacak bir şey yok

// Bütün ağ arayüzlerinde `port`u dinleyen soket. Başarısızsa MR_NO_SOCKET;
// sebebi `error`a yazılır.
mr_socket mr_tcp_listen(int port, int backlog, char* error, size_t error_size);

// Sıradaki bağlantıyı kabul eder. Başarısızsa MR_NO_SOCKET döner ve
// `*transient` sebebi söyler:
//   MR_ACCEPT_FATAL      kalıcı hata
//   MR_ACCEPT_NONE       bekleyen bağlantı yok (engellemeyen dinleyici) ya da
//                        bağlantı kabul edilmeden koptu: şimdilik yapacak bir şey yok
//   MR_ACCEPT_EXHAUSTED  tanıtıcı ya da bellek bir an için tükendi: biraz sonra
//                        yeniden denenmeli
#define MR_ACCEPT_FATAL     0
#define MR_ACCEPT_NONE      1
#define MR_ACCEPT_EXHAUSTED 2
mr_socket mr_tcp_accept(mr_socket server, int* transient);

// Okuma ve yazma için zaman aşımı (saniye; engelleyen kipte).
void mr_socket_set_timeout(mr_socket s, int seconds);

// Engellemeyen kip: okuma / yazma beklemez, yapılamıyorsa hemen döner.
void mr_socket_set_nonblocking(mr_socket s);

// Bütün baytları gönderir; karşı taraf kapattıysa, hata olduysa ya da
// `timeout_seconds` içinde gönderilemediyse 0. Engellemeyen sokette de çalışır
// (yazılabilir olmasını bekler).
int mr_socket_send_all(mr_socket s, const char* data, size_t length, int timeout_seconds);

// En fazla `capacity` bayt okur: okunan bayt sayısı, karşı taraf kapattıysa 0,
// zaman aşımında MR_RECV_TIMEOUT, engellemeyen kipte okunacak bir şey yoksa
// MR_RECV_WOULD_BLOCK, başka bir hatada MR_RECV_ERROR.
int64_t mr_socket_recv(mr_socket s, char* buffer, size_t capacity);

// Birbirine bağlı iki soket (aynı süreçte): birine yazılan diğerinden okunur.
// poll'da bekleyen bir iş parçacığını başka bir iş parçacığından uyandırmak
// için. Başarılıysa 1.
int mr_socket_pair(mr_socket pair[2]);

// --- Birden çok soketi aynı anda bekleme: poll (POSIX) / WSAPoll (Windows) ---

#define MR_POLL_IN  1   // okunacak veri var (ya da bağlantı geldi / kapandı)
#define MR_POLL_OUT 2   // yazılabilir
#define MR_POLL_ERR 4   // hata ya da karşı taraf kapattı (yalnızca revents)

typedef struct {
    mr_socket fd;
    short events;   // beklenenler: MR_POLL_IN | MR_POLL_OUT
    short revents;  // olanlar
} mr_pollfd;

// En fazla `timeout_ms` milisaniye (-1: sonsuz) bekler. Hazır soket sayısı;
// sinyalle bölündüyse 0, hatada -1.
int mr_poll(mr_pollfd* fds, size_t count, int timeout_ms);

// Yazma yönünü kapatıp (karşı taraf yanıtın bittiğini anlar) soketi bırakır.
void mr_socket_close(mr_socket s);

// Bu iş parçacığındaki son soket hatasının açıklaması.
void mr_socket_error_text(char* buffer, size_t size);

#endif // MARLIN_PLATFORM_H
