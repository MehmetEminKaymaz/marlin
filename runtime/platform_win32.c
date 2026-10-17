// platform.h'nin Windows uygulaması.
//
// Windows'un metin API'leri UTF-16'dır; runtime ise her yerde UTF-8 kullanır.
// Yollar, ortam değişkenleri ve argümanlar sınırda çevrilir (utf8_to_wide,
// wide_to_utf8). Konsol da başlangıçta UTF-8'e alınır; aksi halde print'in
// yazdığı Türkçe harfler bozuk görünürdü.
//
// Bağlanacak sistem kütüphaneleri: ws2_32 (soketler), shell32 (argümanlar).

#ifdef _WIN32

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include "platform.h"

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <shellapi.h>

#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#ifdef _MSC_VER
#pragma comment(lib, "ws2_32")
#pragma comment(lib, "shell32")
#endif

_Static_assert(sizeof(mr_mutex) == sizeof(SRWLOCK), "mr_mutex SRWLOCK'u tutmali");
_Static_assert(sizeof(mr_cond) == sizeof(CONDITION_VARIABLE), "mr_cond CONDITION_VARIABLE'i tutmali");
_Static_assert(sizeof(mr_socket) >= sizeof(SOCKET), "mr_socket SOCKET'i tutmali");

// ---------------------------------------------------------------------------
// UTF-8 <-> UTF-16
// ---------------------------------------------------------------------------

// Sonuç çağıranındır (free). Geçersiz UTF-8'de NULL.
static wchar_t* utf8_to_wide(const char* text) {
    int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text, -1, NULL, 0);
    if (n <= 0) return NULL;
    wchar_t* wide = malloc((size_t)n * sizeof *wide);
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text, -1, wide, n);
    return wide;
}

static char* wide_to_utf8(const wchar_t* wide) {
    int n = WideCharToMultiByte(CP_UTF8, 0, wide, -1, NULL, 0, NULL, NULL);
    if (n <= 0) return NULL;
    char* text = malloc((size_t)n);
    WideCharToMultiByte(CP_UTF8, 0, wide, -1, text, n, NULL, NULL);
    return text;
}

// ---------------------------------------------------------------------------
// aligned_alloc
// ---------------------------------------------------------------------------

// MLIR'in görev ve paralel döngü indirmesi coroutine çerçevelerini C11'in
// aligned_alloc'u ile ayırır ve düz free ile bırakır (AsyncToLLVM). Windows'un
// C kütüphanesinde aligned_alloc yoktur; _aligned_malloc ise _aligned_free
// ister. malloc Windows x64 ve ARM64'te 16 bayta hizalı adres verir; çerçeve
// hizalaması da en fazla budur (askıya alma noktasında vektör değeri
// yaşamaz). Daha büyüğü istenirse free ile uyumlu verilemez: program durur.
void* aligned_alloc(size_t alignment, size_t size) {
    if (alignment > 16) {
        fprintf(stderr, "calisma zamani hatasi: aligned_alloc: %zu baytlik hizalama Windows'ta "
                        "desteklenmiyor (en fazla 16)\n", alignment);
        fflush(stderr);
        mr_exit_now(1);
    }
    return malloc(size);
}

// ---------------------------------------------------------------------------
// Program
// ---------------------------------------------------------------------------

// Argümanlar program başlamadan önce bir kez UTF-8'e çevrilir.
static int saved_argc;
static char** saved_argv;

__attribute__((constructor)) static void start_up(void) {
    int argc = 0;
    wchar_t** wide = CommandLineToArgvW(GetCommandLineW(), &argc);
    saved_argv = calloc((size_t)argc + 1, sizeof *saved_argv);
    for (int i = 0; i < argc; ++i) saved_argv[i] = wide_to_utf8(wide[i]);
    saved_argc = argc;
    LocalFree(wide);

    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);
}

_Noreturn void mr_exit_now(int code) {
    TerminateProcess(GetCurrentProcess(), (UINT)code);
    _Exit(code); // (TerminateProcess kendi sürecinde dönmez)
}

void* mr_library_open(const char* name) {
    wchar_t* wide = utf8_to_wide(name);
    if (!wide) return NULL;
    // (Yalnızca sistem dizinlerinde ve programın yanında aranır; çalışma
    // dizinindeki aynı adlı bir DLL yüklenmez.)
    HMODULE module = LoadLibraryExW(wide, NULL, LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    free(wide);
    return (void*)module;
}

void* mr_library_symbol(void* library, const char* name) {
    return (void*)GetProcAddress((HMODULE)library, name);
}

int mr_arg_count(void) { return saved_argc; }
const char* mr_arg(int index) { return saved_argv[index]; }

char* mr_getenv(const char* name) {
    wchar_t* wname = utf8_to_wide(name);
    if (!wname) return NULL;
    char* result = NULL;
    DWORD n = GetEnvironmentVariableW(wname, NULL, 0);
    if (n > 0) {
        wchar_t* value = malloc((size_t)n * sizeof *value);
        if (GetEnvironmentVariableW(wname, value, n) < n) result = wide_to_utf8(value);
        free(value);
    }
    free(wname);
    return result;
}

// ---------------------------------------------------------------------------
// Standart akışlar, dosyalar
// ---------------------------------------------------------------------------

void mr_lock_stream(FILE* stream) { _lock_file(stream); }
void mr_unlock_stream(FILE* stream) { _unlock_file(stream); }

FILE* mr_fopen(const char* path, const char* mode) {
    wchar_t* wpath = utf8_to_wide(path);
    wchar_t* wmode = utf8_to_wide(mode);
    FILE* file = wpath && wmode ? _wfopen(wpath, wmode) : NULL;
    free(wpath);
    free(wmode);
    return file;
}

int mr_remove(const char* path) {
    wchar_t* wpath = utf8_to_wide(path);
    const int ok = wpath && _wremove(wpath) == 0;
    free(wpath);
    return ok;
}

int mr_file_exists(const char* path) {
    wchar_t* wpath = utf8_to_wide(path);
    const int ok = wpath && GetFileAttributesW(wpath) != INVALID_FILE_ATTRIBUTES;
    free(wpath);
    return ok;
}

// ---------------------------------------------------------------------------
// Zaman
// ---------------------------------------------------------------------------

double mr_monotonic_seconds(void) {
    LARGE_INTEGER counter, frequency;
    QueryPerformanceCounter(&counter);
    QueryPerformanceFrequency(&frequency);
    return (double)counter.QuadPart / (double)frequency.QuadPart;
}

double mr_wall_seconds(void) {
    // FILETIME: 1601'den beri 100 ns'lik adımlar.
    FILETIME now;
    GetSystemTimePreciseAsFileTime(&now);
    const uint64_t ticks = ((uint64_t)now.dwHighDateTime << 32) | now.dwLowDateTime;
    const uint64_t unix_epoch = 116444736000000000ULL;  // 1970-01-01, aynı birimle
    return (double)(ticks - unix_epoch) / 1e7;
}

void mr_sleep_seconds(double seconds) {
    if (seconds <= 0) return;
    const double ms = seconds * 1000.0;
    Sleep(ms >= (double)INFINITE ? INFINITE - 1 : (DWORD)(ms + 0.999));
}

// ---------------------------------------------------------------------------
// Kilit ve koşul değişkeni (SRWLOCK ve CONDITION_VARIABLE bırakılmaz)
// ---------------------------------------------------------------------------

void mr_mutex_init(mr_mutex* m) { InitializeSRWLock((PSRWLOCK)m); }
void mr_mutex_destroy(mr_mutex* m) { (void)m; }
void mr_mutex_lock(mr_mutex* m) { AcquireSRWLockExclusive((PSRWLOCK)m); }
void mr_mutex_unlock(mr_mutex* m) { ReleaseSRWLockExclusive((PSRWLOCK)m); }

void mr_cond_init(mr_cond* c) { InitializeConditionVariable((PCONDITION_VARIABLE)c); }
void mr_cond_destroy(mr_cond* c) { (void)c; }
void mr_cond_wait(mr_cond* c, mr_mutex* m) {
    SleepConditionVariableSRW((PCONDITION_VARIABLE)c, (PSRWLOCK)m, INFINITE, 0);
}
void mr_cond_signal(mr_cond* c) { WakeConditionVariable((PCONDITION_VARIABLE)c); }

// ---------------------------------------------------------------------------
// TCP soketleri (Winsock)
// ---------------------------------------------------------------------------

static INIT_ONCE winsock_once = INIT_ONCE_STATIC_INIT;

static BOOL CALLBACK start_winsock(PINIT_ONCE once, PVOID parameter, PVOID* context) {
    (void)once, (void)parameter, (void)context;
    WSADATA data;
    return WSAStartup(MAKEWORD(2, 2), &data) == 0;
}

static int ensure_winsock(void) { return InitOnceExecuteOnce(&winsock_once, start_winsock, NULL, NULL); }

void mr_socket_error_text(char* buffer, size_t size) {
    const DWORD code = (DWORD)WSAGetLastError();
    char message[256];
    DWORD n = FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, NULL, code,
                             MAKELANGID(LANG_ENGLISH, SUBLANG_DEFAULT), message, sizeof message, NULL);
    while (n > 0 && (message[n - 1] == '\n' || message[n - 1] == '\r' || message[n - 1] == '.')) --n;
    message[n] = '\0';
    snprintf(buffer, size, "%s (WSA %lu)", n ? message : "bilinmeyen hata", (unsigned long)code);
}

mr_socket mr_tcp_listen(int port, int backlog, char* error, size_t error_size) {
    if (!ensure_winsock()) {
        snprintf(error, error_size, "Winsock baslatilamadi");
        return MR_NO_SOCKET;
    }
    SOCKET s = WSASocketW(AF_INET, SOCK_STREAM, IPPROTO_TCP, NULL, 0, WSA_FLAG_NO_HANDLE_INHERIT);
    if (s == INVALID_SOCKET) goto fail;

    // Windows'ta SO_REUSEADDR başka bir sürecin aynı portu "çalmasına" izin
    // verir; doğrusu SO_EXCLUSIVEADDRUSE. Kapanan bağlantılar beklerken portu
    // yeniden dinlemek Windows'ta zaten engellenmez.
    BOOL yes = TRUE;
    setsockopt(s, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, (const char*)&yes, sizeof yes);

    struct sockaddr_in address;
    memset(&address, 0, sizeof address);
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    address.sin_port = htons((u_short)port);
    if (bind(s, (struct sockaddr*)&address, sizeof address) == SOCKET_ERROR) goto fail;
    if (listen(s, backlog) == SOCKET_ERROR) goto fail;
    return (mr_socket)s;

fail:
    mr_socket_error_text(error, error_size);
    if (s != INVALID_SOCKET) closesocket(s);
    return MR_NO_SOCKET;
}

mr_socket mr_tcp_accept(mr_socket server, int* transient) {
    SOCKET s = accept((SOCKET)server, NULL, NULL);
    if (s == INVALID_SOCKET) {
        const int code = WSAGetLastError();
        if (code == WSAEWOULDBLOCK || code == WSAECONNRESET || code == WSAEINTR)
            *transient = MR_ACCEPT_NONE;
        else if (code == WSAEMFILE || code == WSAENOBUFS)
            *transient = MR_ACCEPT_EXHAUSTED;
        else
            *transient = MR_ACCEPT_FATAL;
        return MR_NO_SOCKET;
    }
    SetHandleInformation((HANDLE)s, HANDLE_FLAG_INHERIT, 0);
    *transient = MR_ACCEPT_FATAL;
    return (mr_socket)s;
}

void mr_socket_set_nonblocking(mr_socket s) {
    u_long yes = 1;
    ioctlsocket((SOCKET)s, FIONBIO, &yes);
}

// Windows'ta socketpair yok: 127.0.0.1 üzerinde geçici bir dinleyiciyle
// birbirine bağlanan iki TCP soketi.
int mr_socket_pair(mr_socket pair[2]) {
    if (!ensure_winsock()) return 0;
    SOCKET listener = WSASocketW(AF_INET, SOCK_STREAM, IPPROTO_TCP, NULL, 0, WSA_FLAG_NO_HANDLE_INHERIT);
    SOCKET a = INVALID_SOCKET, b = INVALID_SOCKET;
    if (listener == INVALID_SOCKET) return 0;
    struct sockaddr_in address;
    int size = sizeof address;
    memset(&address, 0, sizeof address);
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = 0;
    if (bind(listener, (struct sockaddr*)&address, sizeof address) == SOCKET_ERROR ||
        getsockname(listener, (struct sockaddr*)&address, &size) == SOCKET_ERROR ||
        listen(listener, 1) == SOCKET_ERROR)
        goto fail;
    a = WSASocketW(AF_INET, SOCK_STREAM, IPPROTO_TCP, NULL, 0, WSA_FLAG_NO_HANDLE_INHERIT);
    if (a == INVALID_SOCKET || connect(a, (struct sockaddr*)&address, sizeof address) == SOCKET_ERROR) goto fail;
    b = accept(listener, NULL, NULL);
    if (b == INVALID_SOCKET) goto fail;
    SetHandleInformation((HANDLE)b, HANDLE_FLAG_INHERIT, 0);
    closesocket(listener);
    pair[0] = (mr_socket)a;
    pair[1] = (mr_socket)b;
    return 1;
fail:
    if (a != INVALID_SOCKET) closesocket(a);
    closesocket(listener);
    return 0;
}

int mr_poll(mr_pollfd* fds, size_t count, int timeout_ms) {
    WSAPOLLFD small[64];
    WSAPOLLFD* p = count <= 64 ? small : malloc(count * sizeof *p);
    if (!p) return -1;
    for (size_t i = 0; i < count; ++i) {
        p[i].fd = (SOCKET)fds[i].fd;
        p[i].events = (SHORT)(((fds[i].events & MR_POLL_IN) ? POLLRDNORM : 0) |
                              ((fds[i].events & MR_POLL_OUT) ? POLLWRNORM : 0));
        p[i].revents = 0;
    }
    int n = WSAPoll(p, (ULONG)count, timeout_ms);
    for (size_t i = 0; i < count; ++i) {
        const SHORT r = p[i].revents;
        fds[i].revents = (short)(((r & (POLLRDNORM | POLLRDBAND)) ? MR_POLL_IN : 0) |
                                 ((r & POLLWRNORM) ? MR_POLL_OUT : 0) |
                                 ((r & (POLLERR | POLLHUP | POLLNVAL)) ? MR_POLL_ERR : 0));
    }
    if (p != small) free(p);
    return n == SOCKET_ERROR ? -1 : n;
}

void mr_socket_set_timeout(mr_socket s, int seconds) {
    DWORD ms = (DWORD)seconds * 1000;  // Windows'ta zaman aşımı milisaniyedir
    setsockopt((SOCKET)s, SOL_SOCKET, SO_RCVTIMEO, (const char*)&ms, sizeof ms);
    setsockopt((SOCKET)s, SOL_SOCKET, SO_SNDTIMEO, (const char*)&ms, sizeof ms);
}

int mr_socket_send_all(mr_socket s, const char* data, size_t length, int timeout_seconds) {
    const double deadline = mr_monotonic_seconds() + timeout_seconds;
    while (length > 0) {
        const int chunk = length > INT_MAX ? INT_MAX : (int)length;
        const int n = send((SOCKET)s, data, chunk, 0);
        if (n == SOCKET_ERROR) {
            const int code = WSAGetLastError();
            if (code == WSAEINTR) continue;
            if (code != WSAEWOULDBLOCK) return 0;
            // Gönderim tamponu dolu: yazılabilir olana kadar bekle.
            const double left = deadline - mr_monotonic_seconds();
            if (left <= 0) return 0;
            mr_pollfd p = {s, MR_POLL_OUT, 0};
            if (mr_poll(&p, 1, (int)(left * 1000) + 1) < 0) return 0;
            continue;
        }
        data += n;
        length -= (size_t)n;
    }
    return 1;
}

int64_t mr_socket_recv(mr_socket s, char* buffer, size_t capacity) {
    const int chunk = capacity > INT_MAX ? INT_MAX : (int)capacity;
    for (;;) {
        const int n = recv((SOCKET)s, buffer, chunk, 0);
        if (n >= 0) return n;
        const int code = WSAGetLastError();
        if (code == WSAEINTR) continue;
        if (code == WSAEWOULDBLOCK) return MR_RECV_WOULD_BLOCK;
        return code == WSAETIMEDOUT ? MR_RECV_TIMEOUT : MR_RECV_ERROR;
    }
}

void mr_socket_close(mr_socket s) {
    shutdown((SOCKET)s, SD_SEND);
    closesocket((SOCKET)s);
}

#endif // _WIN32
