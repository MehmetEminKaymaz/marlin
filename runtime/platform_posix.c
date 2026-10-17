// platform.h'nin POSIX (Linux, macOS) uygulaması.
//
// Linux'a ve macOS'a özgü yerler #ifdef ile ayrılmıştır:
//   * accept4 ve MSG_NOSIGNAL yalnızca Linux'ta var; macOS'ta soketin
//     SO_NOSIGPIPE seçeneği ve fcntl(FD_CLOEXEC) aynı işi görür.

#ifndef _WIN32

#ifndef _GNU_SOURCE
#define _GNU_SOURCE  // glibc: accept4, flockfile
#endif

#include "platform.h"

#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

// ---------------------------------------------------------------------------
// Program
// ---------------------------------------------------------------------------

// Programın `main`i parametresizdir; argümanlar program başlamadan önce
// buraya kaydedilir. (glibc ve macOS, başlangıç fonksiyonlarına main'in
// argümanlarını verir.)
static int saved_argc;
static char** saved_argv;

__attribute__((constructor)) static void save_arguments(int argc, char** argv) {
    saved_argc = argc;
    saved_argv = argv;
}

int mr_arg_count(void) { return saved_argc; }
const char* mr_arg(int index) { return saved_argv[index]; }

char* mr_getenv(const char* name) {
    const char* value = getenv(name);
    return value ? strdup(value) : NULL;
}

// ---------------------------------------------------------------------------
// Standart akışlar, dosyalar
// ---------------------------------------------------------------------------

void mr_lock_stream(FILE* stream) { flockfile(stream); }
void mr_unlock_stream(FILE* stream) { funlockfile(stream); }

FILE* mr_fopen(const char* path, const char* mode) { return fopen(path, mode); }
int mr_remove(const char* path) { return remove(path) == 0; }
int mr_file_exists(const char* path) { return access(path, F_OK) == 0; }

// ---------------------------------------------------------------------------
// Zaman
// ---------------------------------------------------------------------------

double mr_monotonic_seconds(void) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (double)now.tv_sec + (double)now.tv_nsec / 1e9;
}

double mr_wall_seconds(void) {
    struct timespec now;
    clock_gettime(CLOCK_REALTIME, &now);
    return (double)now.tv_sec + (double)now.tv_nsec / 1e9;
}

void mr_sleep_seconds(double seconds) {
    if (seconds <= 0) return;
    struct timespec duration;
    duration.tv_sec = (time_t)seconds;
    duration.tv_nsec = (long)((seconds - (double)duration.tv_sec) * 1e9);
    // Sinyal uykuyu bölerse kalan süre kadar devam edilir.
    while (nanosleep(&duration, &duration) < 0 && errno == EINTR) {}
}

// ---------------------------------------------------------------------------
// Kilit ve koşul değişkeni
// ---------------------------------------------------------------------------

void mr_mutex_init(mr_mutex* m) { pthread_mutex_init(m, NULL); }
void mr_mutex_destroy(mr_mutex* m) { pthread_mutex_destroy(m); }
void mr_mutex_lock(mr_mutex* m) { pthread_mutex_lock(m); }
void mr_mutex_unlock(mr_mutex* m) { pthread_mutex_unlock(m); }

void mr_cond_init(mr_cond* c) { pthread_cond_init(c, NULL); }
void mr_cond_destroy(mr_cond* c) { pthread_cond_destroy(c); }
void mr_cond_wait(mr_cond* c, mr_mutex* m) { pthread_cond_wait(c, m); }
void mr_cond_signal(mr_cond* c) { pthread_cond_signal(c); }

// ---------------------------------------------------------------------------
// TCP soketleri
// ---------------------------------------------------------------------------

// Linux'ta SIGPIPE her gönderimde MSG_NOSIGNAL ile, macOS'ta soket başına
// SO_NOSIGPIPE ile bastırılır.
#ifdef MSG_NOSIGNAL
#define SEND_FLAGS MSG_NOSIGNAL
#else
#define SEND_FLAGS 0
#endif

static void prepare_socket(int fd) {
#ifndef SOCK_CLOEXEC
    fcntl(fd, F_SETFD, FD_CLOEXEC);
#endif
#ifdef SO_NOSIGPIPE
    int yes = 1;
    setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &yes, sizeof yes);
#endif
    (void)fd;
}

mr_socket mr_tcp_listen(int port, int backlog, char* error, size_t error_size) {
#ifdef SOCK_CLOEXEC
    int fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
#else
    int fd = socket(AF_INET, SOCK_STREAM, 0);
#endif
    if (fd < 0) goto fail;
    prepare_socket(fd);

    // Program yeniden başlatıldığında, kapanan eski bağlantılar beklerken de
    // aynı port hemen dinlenebilsin.
    int yes = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof yes);

    struct sockaddr_in address;
    memset(&address, 0, sizeof address);
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    address.sin_port = htons((uint16_t)port);
    if (bind(fd, (struct sockaddr*)&address, sizeof address) < 0) goto fail;
    if (listen(fd, backlog) < 0) goto fail;
    return fd;

fail:
    snprintf(error, error_size, "%s", strerror(errno));
    if (fd >= 0) close(fd);
    return MR_NO_SOCKET;
}

mr_socket mr_tcp_accept(mr_socket server, int* transient) {
#if defined(__linux__)
    int fd = accept4((int)server, NULL, NULL, SOCK_CLOEXEC);
#else
    int fd = accept((int)server, NULL, NULL);
#endif
    if (fd < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR || errno == ECONNABORTED)
            *transient = MR_ACCEPT_NONE;
        else if (errno == EMFILE || errno == ENFILE || errno == ENOBUFS || errno == ENOMEM)
            *transient = MR_ACCEPT_EXHAUSTED;
        else
            *transient = MR_ACCEPT_FATAL;
        return MR_NO_SOCKET;
    }
#if !defined(__linux__)
    fcntl(fd, F_SETFD, FD_CLOEXEC);
#endif
    prepare_socket(fd);
    *transient = MR_ACCEPT_FATAL;
    return fd;
}

void mr_socket_set_nonblocking(mr_socket s) {
    const int flags = fcntl((int)s, F_GETFL, 0);
    fcntl((int)s, F_SETFL, flags | O_NONBLOCK);
}

static int is_nonblocking(int fd) { return (fcntl(fd, F_GETFL, 0) & O_NONBLOCK) != 0; }

int mr_socket_pair(mr_socket pair[2]) {
    int fds[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, fds) < 0) return 0;
    for (int i = 0; i < 2; ++i) {
        fcntl(fds[i], F_SETFD, FD_CLOEXEC);
        prepare_socket(fds[i]);
        pair[i] = fds[i];
    }
    return 1;
}

int mr_poll(mr_pollfd* fds, size_t count, int timeout_ms) {
    struct pollfd small[64];
    struct pollfd* p = count <= 64 ? small : malloc(count * sizeof *p);
    if (!p) return -1;
    for (size_t i = 0; i < count; ++i) {
        p[i].fd = (int)fds[i].fd;
        p[i].events = (short)(((fds[i].events & MR_POLL_IN) ? POLLIN : 0) |
                              ((fds[i].events & MR_POLL_OUT) ? POLLOUT : 0));
        p[i].revents = 0;
    }
    int n = poll(p, (nfds_t)count, timeout_ms);
    if (n < 0 && errno == EINTR) n = 0;
    for (size_t i = 0; i < count; ++i) {
        const short r = p[i].revents;
        fds[i].revents = (short)(((r & POLLIN) ? MR_POLL_IN : 0) | ((r & POLLOUT) ? MR_POLL_OUT : 0) |
                                 ((r & (POLLERR | POLLHUP | POLLNVAL)) ? MR_POLL_ERR : 0));
    }
    if (p != small) free(p);
    return n;
}

void mr_socket_set_timeout(mr_socket s, int seconds) {
    struct timeval timeout = {seconds, 0};
    setsockopt((int)s, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof timeout);
    setsockopt((int)s, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof timeout);
}

int mr_socket_send_all(mr_socket s, const char* data, size_t length, int timeout_seconds) {
    const double deadline = mr_monotonic_seconds() + timeout_seconds;
    while (length > 0) {
        ssize_t n = send((int)s, data, length, SEND_FLAGS);
        if (n < 0) {
            if (errno == EINTR) continue;
            if (errno != EAGAIN && errno != EWOULDBLOCK) return 0;
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
    for (;;) {
        ssize_t n = recv((int)s, buffer, capacity, 0);
        if (n >= 0) return n;
        if (errno == EINTR) continue;
        if (errno == EAGAIN || errno == EWOULDBLOCK)
            return is_nonblocking((int)s) ? MR_RECV_WOULD_BLOCK : MR_RECV_TIMEOUT;
        return MR_RECV_ERROR;
    }
}

void mr_socket_close(mr_socket s) {
    shutdown((int)s, SHUT_WR);
    close((int)s);
}

void mr_socket_error_text(char* buffer, size_t size) { snprintf(buffer, size, "%s", strerror(errno)); }

#endif // !_WIN32
