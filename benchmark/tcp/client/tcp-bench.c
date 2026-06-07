/* ==========================================================================
 * tcp-bench: cross-platform TCP load generator.
 *
 *   Linux   -> epoll   (readiness / reactor)
 *   macOS   -> kqueue  (readiness / reactor)
 *   Windows -> IOCP    (completion / proactor)
 *
 * Modes: throughput | connrate | memory
 *
 * NOTE: the POSIX (epoll/kqueue) and Windows (IOCP) backends are separate
 * implementations because readiness and completion models have different
 * control flow. Shared code is limited to argument parsing, timing,
 * RSS sampling, and JSON/statistics emission.
 * ========================================================================== */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>

/* -------------------------------------------------------------------------- */
/* platform headers + socket primitive abstraction                            */
/* -------------------------------------------------------------------------- */

#if defined(_WIN32)

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <mswsock.h>
#include <windows.h>
#include <psapi.h>

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "mswsock.lib")
#pragma comment(lib, "psapi.lib")

typedef SOCKET sock_t;
#define BENCH_INVALID_SOCK INVALID_SOCKET
typedef int    socklen_t;

#else /* POSIX */

#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <signal.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

typedef int sock_t;
#define BENCH_INVALID_SOCK (-1)

#endif

#define MAX_EVENTS    4096
#define LATENCY_SLOTS 1000000

static const char* g_host = "127.0.0.1";
static int         g_port = 9000;
static volatile bool g_running = true;

/* -------------------------------------------------------------------------- */
/* shared helpers: timing, memory, statistics, output                         */
/* -------------------------------------------------------------------------- */

static uint64_t now_us(void) {
#if defined(_WIN32)
    static LARGE_INTEGER freq;
    if (freq.QuadPart == 0) {
        QueryPerformanceFrequency(&freq);
    }
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return (uint64_t)(c.QuadPart * 1000000ULL / (uint64_t)freq.QuadPart);
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000 + (uint64_t)ts.tv_nsec / 1000;
#endif
}

static long get_rss_kb(void) {
#if defined(_WIN32)
    PROCESS_MEMORY_COUNTERS pmc;
    if (GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc))) {
        return (long)(pmc.WorkingSetSize / 1024);
    }
    return 0;
#elif defined(__linux__)
    FILE* f = fopen("/proc/self/status", "r");
    if (!f) return 0;
    char line[256];
    long rss = 0;
    while (fgets(line, sizeof(line), f)) {
        if (strncmp(line, "VmRSS:", 6) == 0) {
            sscanf(line + 6, "%ld", &rss);
            break;
        }
    }
    fclose(f);
    return rss;
#else
    /* macOS/BSD: ru_maxrss is in bytes on Darwin, KB on Linux. */
    struct rusage ru;
    if (getrusage(RUSAGE_SELF, &ru) != 0) return 0;
#if defined(__APPLE__)
    return ru.ru_maxrss / 1024;
#else
    return ru.ru_maxrss;
#endif
#endif
}

static int cmp_u64(const void* a, const void* b) {
    uint64_t va = *(const uint64_t*)a;
    uint64_t vb = *(const uint64_t*)b;
    if (va < vb) return -1;
    if (va > vb) return 1;
    return 0;
}

static void emit_throughput_json(
    int connected, double elapsed_sec,
    uint64_t msgs_sent, uint64_t msgs_recv,
    uint64_t* latencies, int lat_count) {
    qsort(latencies, (size_t)lat_count, sizeof(uint64_t), cmp_u64);
    uint64_t p50 = lat_count > 0 ? latencies[lat_count / 2] : 0;
    uint64_t p99 = lat_count > 0 ? latencies[(int)(lat_count * 0.99)] : 0;
    uint64_t mx  = lat_count > 0 ? latencies[lat_count - 1] : 0;

    printf("{\n");
    printf("  \"connections\": %d,\n", connected);
    printf("  \"duration_sec\": %.2f,\n", elapsed_sec);
    printf("  \"messages_sent\": %" PRIu64 ",\n", msgs_sent);
    printf("  \"messages_recv\": %" PRIu64 ",\n", msgs_recv);
    printf("  \"throughput_msg_per_sec\": %.0f,\n",
           elapsed_sec > 0 ? (double)msgs_recv / elapsed_sec : 0.0);
    printf("  \"latency_p50_us\": %" PRIu64 ",\n", p50);
    printf("  \"latency_p99_us\": %" PRIu64 ",\n", p99);
    printf("  \"latency_max_us\": %" PRIu64 ",\n", mx);
    printf("  \"memory_rss_kb\": %ld\n", get_rss_kb());
    printf("}\n");
}

static void emit_connrate_json(
    double elapsed_sec, int concurrency,
    uint64_t connects_ok, uint64_t connects_fail) {
    printf("{\n");
    printf("  \"benchmark\": \"connrate\",\n");
    printf("  \"duration_sec\": %.2f,\n", elapsed_sec);
    printf("  \"concurrency\": %d,\n", concurrency);
    printf("  \"total_connects\": %" PRIu64 ",\n", connects_ok);
    printf("  \"failed_connects\": %" PRIu64 ",\n", connects_fail);
    printf("  \"connects_per_sec\": %.0f\n",
           elapsed_sec > 0 ? (double)connects_ok / elapsed_sec : 0.0);
    printf("}\n");
}

static void emit_memory_json(
    int target_conns, int established, long client_rss_kb) {
    printf("{\n");
    printf("  \"benchmark\": \"memory\",\n");
    printf("  \"target_connections\": %d,\n", target_conns);
    printf("  \"established_connections\": %d,\n", established);
    printf("  \"client_rss_kb\": %ld\n", client_rss_kb);
    printf("}\n");
}

/* parse common -h/-p; per-mode opts parsed by callers */
static void parse_host_port(int argc, char** argv) {
    for (int i = 0; i < argc; i++) {
        if (strcmp(argv[i], "-h") == 0 && i + 1 < argc)
            g_host = argv[++i];
        else if (strcmp(argv[i], "-p") == 0 && i + 1 < argc)
            g_port = atoi(argv[++i]);
    }
}

static void make_addr(struct sockaddr_in* sa) {
    memset(sa, 0, sizeof(*sa));
    sa->sin_family = AF_INET;
    sa->sin_port   = htons((uint16_t)g_port);
    inet_pton(AF_INET, g_host, &sa->sin_addr);
}

#if !defined(_WIN32)
/* ==========================================================================
 * POSIX backend: epoll (Linux) / kqueue (macOS, *BSD) — readiness model.
 * ========================================================================== */

typedef struct {
    sock_t fd;
    bool   readable;
    bool   writable;
    bool   error;
} poll_ev_t;

#if defined(__linux__)
#include <sys/epoll.h>

static int poller_create(void) { return epoll_create1(0); }

static void poller_add(int pfd, sock_t fd, bool want_rd, bool want_wr) {
    struct epoll_event ev = { .data.fd = fd, .events = EPOLLET };
    if (want_rd) ev.events |= EPOLLIN;
    if (want_wr) ev.events |= EPOLLOUT;
    epoll_ctl(pfd, EPOLL_CTL_ADD, fd, &ev);
}

static void poller_del(int pfd, sock_t fd) {
    epoll_ctl(pfd, EPOLL_CTL_DEL, fd, NULL);
}

static int poller_wait(int pfd, poll_ev_t* out, int max, int timeout_ms) {
    struct epoll_event events[MAX_EVENTS];
    if (max > MAX_EVENTS) max = MAX_EVENTS;
    int nev = epoll_wait(pfd, events, max, timeout_ms);
    for (int i = 0; i < nev; i++) {
        out[i].fd       = events[i].data.fd;
        out[i].readable = (events[i].events & EPOLLIN) != 0;
        out[i].writable = (events[i].events & EPOLLOUT) != 0;
        out[i].error    = (events[i].events & (EPOLLERR | EPOLLHUP)) != 0;
    }
    return nev;
}

#else /* kqueue */
#include <sys/event.h>

static int poller_create(void) { return kqueue(); }

static void poller_add(int pfd, sock_t fd, bool want_rd, bool want_wr) {
    struct kevent ch[2];
    int n = 0;
    if (want_rd)
        EV_SET(&ch[n++], fd, EVFILT_READ, EV_ADD | EV_CLEAR, 0, 0, NULL);
    if (want_wr)
        EV_SET(&ch[n++], fd, EVFILT_WRITE, EV_ADD | EV_CLEAR, 0, 0, NULL);
    if (n) kevent(pfd, ch, n, NULL, 0, NULL);
}

static void poller_del(int pfd, sock_t fd) {
    struct kevent ch[2];
    EV_SET(&ch[0], fd, EVFILT_READ, EV_DELETE, 0, 0, NULL);
    EV_SET(&ch[1], fd, EVFILT_WRITE, EV_DELETE, 0, 0, NULL);
    kevent(pfd, ch, 2, NULL, 0, NULL);
}

static int poller_wait(int pfd, poll_ev_t* out, int max, int timeout_ms) {
    struct kevent events[MAX_EVENTS];
    if (max > MAX_EVENTS) max = MAX_EVENTS;
    struct timespec ts = {
        .tv_sec  = timeout_ms / 1000,
        .tv_nsec = (long)(timeout_ms % 1000) * 1000000L,
    };
    int nev = kevent(pfd, NULL, 0, events, max, &ts);
    /* Merge READ/WRITE filters that share the same fd into one event. */
    int out_n = 0;
    for (int i = 0; i < nev; i++) {
        sock_t fd = (sock_t)events[i].ident;
        int slot = -1;
        for (int j = 0; j < out_n; j++) {
            if (out[j].fd == fd) { slot = j; break; }
        }
        if (slot < 0) {
            slot = out_n++;
            out[slot].fd = fd;
            out[slot].readable = false;
            out[slot].writable = false;
            out[slot].error = false;
        }
        if (events[i].filter == EVFILT_READ)  out[slot].readable = true;
        if (events[i].filter == EVFILT_WRITE) out[slot].writable = true;
        if (events[i].flags & EV_EOF)         out[slot].error = true;
    }
    return out_n;
}
#endif

static void set_nonblocking(sock_t fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

static void sig_handler(int sig) {
    (void)sig;
    g_running = false;
}

/* ----- THROUGHPUT (POSIX) ----- */

typedef struct {
    sock_t   fd;
    uint64_t send_ts;
    size_t   recv_accum;
    bool     connected;
    bool     awaiting_echo;
} conn_t;

static void run_throughput(int argc, char** argv) {
    int target_conns = 1000;
    int duration_sec = 30;
    int payload_size = 64;

    for (int i = 0; i < argc; i++) {
        if (strcmp(argv[i], "-n") == 0 && i + 1 < argc)
            target_conns = atoi(argv[++i]);
        else if (strcmp(argv[i], "-d") == 0 && i + 1 < argc)
            duration_sec = atoi(argv[++i]);
        else if (strcmp(argv[i], "-s") == 0 && i + 1 < argc)
            payload_size = atoi(argv[++i]);
    }
    parse_host_port(argc, argv);

    if (payload_size < 1) payload_size = 1;
    if (payload_size > 65536) payload_size = 65536;

    int max_fds = target_conns + 100;
    conn_t* conns = calloc((size_t)max_fds, sizeof(conn_t));
    uint64_t* latencies = malloc(LATENCY_SLOTS * sizeof(uint64_t));
    int lat_count = 0;
    int connected = 0;
    int failed = 0;
    uint64_t msgs_sent = 0;
    uint64_t msgs_recv = 0;

    int epfd = poller_create();

    struct sockaddr_in sa;
    make_addr(&sa);

    fprintf(stderr, "connecting %d to %s:%d...\n", target_conns, g_host, g_port);

    int created = 0;
    while (created < target_conns) {
        int batch = 1000;
        if (created + batch > target_conns) batch = target_conns - created;
        for (int i = 0; i < batch; i++) {
            sock_t fd = socket(AF_INET, SOCK_STREAM, 0);
            if (fd < 0) break;
            set_nonblocking(fd);
            int yes = 1;
            setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, (void*)&yes, sizeof(yes));
            int rc = connect(fd, (struct sockaddr*)&sa, sizeof(sa));
            if (rc < 0 && errno != EINPROGRESS) { close(fd); break; }
            poller_add(epfd, fd, true, true);
            conns[fd].fd = fd;
            conns[fd].connected = false;
            conns[fd].awaiting_echo = false;
            created++;
        }
        poll_ev_t events[MAX_EVENTS];
        int nev = poller_wait(epfd, events, MAX_EVENTS, 10);
        for (int i = 0; i < nev; i++) {
            conn_t* c = &conns[events[i].fd];
            if (!c->connected && events[i].writable) {
                int err = 0; socklen_t len = sizeof(err);
                getsockopt(c->fd, SOL_SOCKET, SO_ERROR, &err, &len);
                if (err == 0) { c->connected = true; connected++; }
                else failed++;
            }
        }
    }

    while (connected + failed < created) {
        poll_ev_t events[MAX_EVENTS];
        int nev = poller_wait(epfd, events, MAX_EVENTS, 200);
        for (int i = 0; i < nev; i++) {
            conn_t* c = &conns[events[i].fd];
            if (!c->connected && events[i].writable) {
                int err = 0; socklen_t len = sizeof(err);
                getsockopt(c->fd, SOL_SOCKET, SO_ERROR, &err, &len);
                if (err == 0) { c->connected = true; connected++; }
                else failed++;
            }
        }
    }

    fprintf(stderr, "connected: %d / %d (failed: %d), starting echo benchmark for %ds...\n",
            connected, target_conns, failed, duration_sec);

    char payload[65536];
    memset(payload, 'A', (size_t)payload_size);

    for (int fd = 0; fd < max_fds; fd++) {
        if (conns[fd].connected) {
            conns[fd].send_ts = now_us();
            ssize_t n = write(fd, payload, (size_t)payload_size);
            if (n == payload_size) { conns[fd].awaiting_echo = true; msgs_sent++; }
        }
    }

    uint64_t start = now_us();
    uint64_t deadline = start + (uint64_t)duration_sec * 1000000;

    while (g_running && now_us() < deadline) {
        poll_ev_t events[MAX_EVENTS];
        int nev = poller_wait(epfd, events, MAX_EVENTS, 100);
        for (int i = 0; i < nev; i++) {
            if (!events[i].readable) continue;
            conn_t* c = &conns[events[i].fd];
            char buf[65536];
            for (;;) {
                ssize_t n = read(c->fd, buf, sizeof(buf));
                if (n <= 0) break;
                c->recv_accum += (size_t)n;
                while (c->recv_accum >= (size_t)payload_size) {
                    uint64_t lat = now_us() - c->send_ts;
                    if (lat_count < LATENCY_SLOTS) latencies[lat_count++] = lat;
                    msgs_recv++;
                    c->recv_accum -= (size_t)payload_size;
                    c->awaiting_echo = false;
                    c->send_ts = now_us();
                    ssize_t w = write(c->fd, payload, (size_t)payload_size);
                    if (w == payload_size) { c->awaiting_echo = true; msgs_sent++; }
                }
            }
        }
    }

    double elapsed_sec = (double)(now_us() - start) / 1000000.0;
    emit_throughput_json(connected, elapsed_sec, msgs_sent, msgs_recv,
                         latencies, lat_count);

    struct linger lg = { .l_onoff = 1, .l_linger = 0 };
    for (int fd = 0; fd < max_fds; fd++) {
        if (conns[fd].fd > 0) {
            setsockopt(fd, SOL_SOCKET, SO_LINGER, (void*)&lg, sizeof(lg));
            close(fd);
        }
    }
    close(epfd);
    free(conns);
    free(latencies);
}

/* ----- CONNRATE (POSIX) ----- */

static void run_connrate(int argc, char** argv) {
    int concurrency  = 256;
    int duration_sec = 10;

    for (int i = 0; i < argc; i++) {
        if (strcmp(argv[i], "-c") == 0 && i + 1 < argc)
            concurrency = atoi(argv[++i]);
        else if (strcmp(argv[i], "-d") == 0 && i + 1 < argc)
            duration_sec = atoi(argv[++i]);
    }
    parse_host_port(argc, argv);

    int epfd = poller_create();
    uint64_t connects_ok = 0;
    uint64_t connects_fail = 0;

    struct sockaddr_in sa;
    make_addr(&sa);

    fprintf(stderr, "connrate: %d concurrent to %s:%d for %ds\n",
            concurrency, g_host, g_port, duration_sec);

    for (int i = 0; i < concurrency; i++) {
        sock_t fd = socket(AF_INET, SOCK_STREAM, 0);
        if (fd < 0) break;
        set_nonblocking(fd);
        int rc = connect(fd, (struct sockaddr*)&sa, sizeof(sa));
        if (rc < 0 && errno != EINPROGRESS) { close(fd); break; }
        poller_add(epfd, fd, false, true);
    }

    uint64_t start = now_us();
    uint64_t deadline = start + (uint64_t)duration_sec * 1000000;
    poll_ev_t events[MAX_EVENTS];

    while (g_running && now_us() < deadline) {
        int nev = poller_wait(epfd, events, MAX_EVENTS, 10);
        for (int i = 0; i < nev; i++) {
            sock_t fd = events[i].fd;

            if (events[i].error) {
                poller_del(epfd, fd);
                close(fd);
                connects_fail++;
                sock_t nfd = socket(AF_INET, SOCK_STREAM, 0);
                if (nfd >= 0) {
                    set_nonblocking(nfd);
                    int rc = connect(nfd, (struct sockaddr*)&sa, sizeof(sa));
                    if (rc < 0 && errno != EINPROGRESS) { close(nfd); continue; }
                    poller_add(epfd, nfd, false, true);
                }
                continue;
            }

            if (events[i].writable) {
                int err = 0; socklen_t len = sizeof(err);
                getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &len);
                poller_del(epfd, fd);

                struct linger lg = { .l_onoff = 1, .l_linger = 0 };
                if (err == 0) {
                    connects_ok++;
                    setsockopt(fd, SOL_SOCKET, SO_LINGER, (void*)&lg, sizeof(lg));
                } else {
                    connects_fail++;
                }
                close(fd);

                sock_t nfd = socket(AF_INET, SOCK_STREAM, 0);
                if (nfd >= 0) {
                    set_nonblocking(nfd);
                    int rc = connect(nfd, (struct sockaddr*)&sa, sizeof(sa));
                    if (rc < 0 && errno != EINPROGRESS) { close(nfd); continue; }
                    poller_add(epfd, nfd, false, true);
                }
            }
        }
    }

    double elapsed_sec = (double)(now_us() - start) / 1000000.0;
    emit_connrate_json(elapsed_sec, concurrency, connects_ok, connects_fail);
    close(epfd);
}

/* ----- MEMORY (POSIX) ----- */

static void run_memory(int argc, char** argv) {
    int target_conns = 10000;
    int hold_sec     = 5;

    for (int i = 0; i < argc; i++) {
        if (strcmp(argv[i], "-n") == 0 && i + 1 < argc)
            target_conns = atoi(argv[++i]);
        else if (strcmp(argv[i], "-w") == 0 && i + 1 < argc)
            hold_sec = atoi(argv[++i]);
    }
    parse_host_port(argc, argv);

    int epfd = poller_create();
    int connected = 0;
    int failed = 0;

    struct sockaddr_in sa;
    make_addr(&sa);

    fprintf(stderr, "memory: connecting %d to %s:%d...\n",
            target_conns, g_host, g_port);

    int created = 0;
    while (created < target_conns) {
        int batch = 1000;
        if (created + batch > target_conns) batch = target_conns - created;
        for (int i = 0; i < batch; i++) {
            sock_t fd = socket(AF_INET, SOCK_STREAM, 0);
            if (fd < 0) break;
            set_nonblocking(fd);
            int yes = 1;
            setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, (void*)&yes, sizeof(yes));
            int rc = connect(fd, (struct sockaddr*)&sa, sizeof(sa));
            if (rc < 0 && errno != EINPROGRESS) { close(fd); break; }
            poller_add(epfd, fd, false, true);
            created++;
        }
        poll_ev_t events[MAX_EVENTS];
        int nev = poller_wait(epfd, events, MAX_EVENTS, 10);
        for (int i = 0; i < nev; i++) {
            if (events[i].writable) {
                int err = 0; socklen_t len = sizeof(err);
                getsockopt(events[i].fd, SOL_SOCKET, SO_ERROR, &err, &len);
                if (err == 0) connected++;
                else failed++;
            }
        }
    }

    while (connected + failed < created) {
        poll_ev_t events[MAX_EVENTS];
        int nev = poller_wait(epfd, events, MAX_EVENTS, 200);
        for (int i = 0; i < nev; i++) {
            if (events[i].writable) {
                int err = 0; socklen_t len = sizeof(err);
                getsockopt(events[i].fd, SOL_SOCKET, SO_ERROR, &err, &len);
                if (err == 0) connected++;
                else failed++;
            }
        }
    }

    fprintf(stderr, "READY %d/%d (failed: %d)\n", connected, target_conns, failed);
    sleep((unsigned)hold_sec);

    emit_memory_json(target_conns, connected, get_rss_kb());

    struct linger lg = { .l_onoff = 1, .l_linger = 0 };
    int max_fds = target_conns + 100;
    for (int fd = 3; fd < max_fds + 3; fd++) {
        setsockopt(fd, SOL_SOCKET, SO_LINGER, (void*)&lg, sizeof(lg));
        close(fd);
    }
    close(epfd);
}

static void platform_init(void) {
    signal(SIGINT, sig_handler);
    signal(SIGPIPE, SIG_IGN);
    struct rlimit rl = { .rlim_cur = 200000, .rlim_max = 200000 };
    setrlimit(RLIMIT_NOFILE, &rl);
}

static void platform_cleanup(void) {}

#endif /* !_WIN32 */

#if defined(_WIN32)
/* ==========================================================================
 * Windows backend: IOCP (completion / proactor model).
 *
 * Unlike epoll/kqueue (which report readiness), IOCP reports completion of
 * previously-issued overlapped operations. So instead of "wait for writable
 * then write", we POST a WSASend/WSARecv/ConnectEx and the completion port
 * hands the op back when the kernel has finished it.
 *
 * Each socket carries one outstanding-op context per direction. A single
 * thread drives GetQueuedCompletionStatus in a loop; this mirrors the
 * single-threaded client design of the POSIX side (the *server* is what we
 * are benchmarking, not the client).
 * ========================================================================== */

typedef enum { OP_CONNECT, OP_RECV, OP_SEND } op_kind_t;

typedef struct conn_s conn_t;

typedef struct {
    OVERLAPPED ov;       /* must be first */
    op_kind_t  kind;
    conn_t*    conn;
} io_op_t;

struct conn_s {
    sock_t   sock;
    io_op_t  recv_op;
    io_op_t  send_op;
    io_op_t  connect_op;
    char     send_buf[65536];
    char     recv_buf[65536];
    WSABUF   wsa_recv;
    WSABUF   wsa_send;
    uint64_t send_ts;
    size_t   recv_accum;
    bool     connected;
};

static LPFN_CONNECTEX g_ConnectEx = NULL;

static void load_connectex(sock_t s) {
    GUID guid = WSAID_CONNECTEX;
    DWORD bytes = 0;
    WSAIoctl(s, SIO_GET_EXTENSION_FUNCTION_POINTER,
             &guid, sizeof(guid),
             &g_ConnectEx, sizeof(g_ConnectEx),
             &bytes, NULL, NULL);
}

static sock_t make_socket(void) {
    sock_t s = WSASocketW(AF_INET, SOCK_STREAM, IPPROTO_TCP,
                          NULL, 0, WSA_FLAG_OVERLAPPED);
    if (s != INVALID_SOCKET) {
        int yes = 1;
        setsockopt(s, IPPROTO_TCP, TCP_NODELAY,
                   (const char*)&yes, sizeof(yes));
    }
    return s;
}

/* ConnectEx requires the socket be bound first. */
static bool bind_any(sock_t s) {
    struct sockaddr_in local;
    memset(&local, 0, sizeof(local));
    local.sin_family = AF_INET;
    local.sin_addr.s_addr = INADDR_ANY;
    local.sin_port = 0;
    return bind(s, (struct sockaddr*)&local, sizeof(local)) == 0;
}

static bool post_connect(HANDLE iocp, conn_t* c, struct sockaddr_in* sa) {
    if (!bind_any(c->sock)) return false;
    if (!CreateIoCompletionPort((HANDLE)c->sock, iocp, (ULONG_PTR)c, 0))
        return false;
    memset(&c->connect_op.ov, 0, sizeof(OVERLAPPED));
    c->connect_op.kind = OP_CONNECT;
    c->connect_op.conn = c;
    DWORD sent = 0;
    BOOL ok = g_ConnectEx(c->sock, (struct sockaddr*)sa, sizeof(*sa),
                          NULL, 0, &sent, &c->connect_op.ov);
    if (!ok && WSAGetLastError() != ERROR_IO_PENDING) {
        return false;
    }
    return true;
}

static bool post_recv(conn_t* c) {
    memset(&c->recv_op.ov, 0, sizeof(OVERLAPPED));
    c->recv_op.kind = OP_RECV;
    c->recv_op.conn = c;
    c->wsa_recv.buf = c->recv_buf;
    c->wsa_recv.len = sizeof(c->recv_buf);
    DWORD flags = 0, recvd = 0;
    int rc = WSARecv(c->sock, &c->wsa_recv, 1, &recvd, &flags,
                     &c->recv_op.ov, NULL);
    if (rc == SOCKET_ERROR && WSAGetLastError() != WSA_IO_PENDING)
        return false;
    return true;
}

static bool post_send(conn_t* c, int len) {
    memset(&c->send_op.ov, 0, sizeof(OVERLAPPED));
    c->send_op.kind = OP_SEND;
    c->send_op.conn = c;
    c->wsa_send.buf = c->send_buf;
    c->wsa_send.len = (ULONG)len;
    DWORD sent = 0;
    int rc = WSASend(c->sock, &c->wsa_send, 1, &sent, 0,
                     &c->send_op.ov, NULL);
    if (rc == SOCKET_ERROR && WSAGetLastError() != WSA_IO_PENDING)
        return false;
    return true;
}

/* ----- THROUGHPUT (IOCP) ----- */

static void run_throughput(int argc, char** argv) {
    int target_conns = 1000;
    int duration_sec = 30;
    int payload_size = 64;

    for (int i = 0; i < argc; i++) {
        if (strcmp(argv[i], "-n") == 0 && i + 1 < argc)
            target_conns = atoi(argv[++i]);
        else if (strcmp(argv[i], "-d") == 0 && i + 1 < argc)
            duration_sec = atoi(argv[++i]);
        else if (strcmp(argv[i], "-s") == 0 && i + 1 < argc)
            payload_size = atoi(argv[++i]);
    }
    parse_host_port(argc, argv);

    if (payload_size < 1) payload_size = 1;
    if (payload_size > 65536) payload_size = 65536;

    HANDLE iocp = CreateIoCompletionPort(INVALID_HANDLE_VALUE, NULL, 0, 1);
    conn_t* conns = calloc((size_t)target_conns, sizeof(conn_t));
    uint64_t* latencies = malloc(LATENCY_SLOTS * sizeof(uint64_t));
    int lat_count = 0;
    int connected = 0, failed = 0, completed_conn = 0;
    uint64_t msgs_sent = 0, msgs_recv = 0;

    struct sockaddr_in sa;
    make_addr(&sa);

    sock_t probe = make_socket();
    load_connectex(probe);
    closesocket(probe);

    fprintf(stderr, "connecting %d to %s:%d...\n", target_conns, g_host, g_port);

    for (int i = 0; i < target_conns; i++) {
        conns[i].sock = make_socket();
        if (conns[i].sock == INVALID_SOCKET) { failed++; completed_conn++; continue; }
        if (!post_connect(iocp, &conns[i], &sa)) { failed++; completed_conn++; }
    }

    /* drain connect completions */
    while (completed_conn < target_conns) {
        DWORD bytes = 0; ULONG_PTR key = 0; LPOVERLAPPED ov = NULL;
        BOOL ok = GetQueuedCompletionStatus(iocp, &bytes, &key, &ov, 1000);
        if (!ov) break; /* timeout */
        io_op_t* op = (io_op_t*)ov;
        conn_t* c = op->conn;
        if (op->kind == OP_CONNECT) {
            completed_conn++;
            if (ok) {
                setsockopt(c->sock, SOL_SOCKET, SO_UPDATE_CONNECT_CONTEXT,
                           NULL, 0);
                c->connected = true;
                connected++;
            } else {
                failed++;
            }
        }
    }

    fprintf(stderr, "connected: %d / %d (failed: %d), starting echo benchmark for %ds...\n",
            connected, target_conns, failed, duration_sec);

    /* prime: post initial send + recv on each connected socket */
    for (int i = 0; i < target_conns; i++) {
        conn_t* c = &conns[i];
        if (!c->connected) continue;
        memset(c->send_buf, 'A', (size_t)payload_size);
        post_recv(c);
        c->send_ts = now_us();
        if (post_send(c, payload_size)) msgs_sent++;
    }

    uint64_t start = now_us();
    uint64_t deadline = start + (uint64_t)duration_sec * 1000000;

    while (g_running && now_us() < deadline) {
        DWORD bytes = 0; ULONG_PTR key = 0; LPOVERLAPPED ov = NULL;
        BOOL ok = GetQueuedCompletionStatus(iocp, &bytes, &key, &ov, 100);
        if (!ov) continue;
        io_op_t* op = (io_op_t*)ov;
        conn_t* c = op->conn;

        if (!ok || (op->kind == OP_RECV && bytes == 0)) {
            continue; /* peer closed or error; leave socket idle */
        }

        if (op->kind == OP_RECV) {
            c->recv_accum += bytes;
            while (c->recv_accum >= (size_t)payload_size) {
                uint64_t lat = now_us() - c->send_ts;
                if (lat_count < LATENCY_SLOTS) latencies[lat_count++] = lat;
                msgs_recv++;
                c->recv_accum -= (size_t)payload_size;
                c->send_ts = now_us();
                if (post_send(c, payload_size)) msgs_sent++;
            }
            post_recv(c);
        }
        /* OP_SEND completions need no action: the matching recv drives
         * the next send. */
    }

    double elapsed_sec = (double)(now_us() - start) / 1000000.0;
    emit_throughput_json(connected, elapsed_sec, msgs_sent, msgs_recv,
                         latencies, lat_count);

    for (int i = 0; i < target_conns; i++) {
        if (conns[i].sock != INVALID_SOCKET) closesocket(conns[i].sock);
    }
    CloseHandle(iocp);
    free(conns);
    free(latencies);
}

/* ----- CONNRATE (IOCP) ----- */

static void run_connrate(int argc, char** argv) {
    int concurrency  = 256;
    int duration_sec = 10;

    for (int i = 0; i < argc; i++) {
        if (strcmp(argv[i], "-c") == 0 && i + 1 < argc)
            concurrency = atoi(argv[++i]);
        else if (strcmp(argv[i], "-d") == 0 && i + 1 < argc)
            duration_sec = atoi(argv[++i]);
    }
    parse_host_port(argc, argv);

    HANDLE iocp = CreateIoCompletionPort(INVALID_HANDLE_VALUE, NULL, 0, 1);
    uint64_t connects_ok = 0, connects_fail = 0;

    struct sockaddr_in sa;
    make_addr(&sa);

    sock_t probe = make_socket();
    load_connectex(probe);
    closesocket(probe);

    fprintf(stderr, "connrate: %d concurrent to %s:%d for %ds\n",
            concurrency, g_host, g_port, duration_sec);

    /* fixed pool of connect contexts, recycled on completion */
    conn_t* pool = calloc((size_t)concurrency, sizeof(conn_t));
    for (int i = 0; i < concurrency; i++) {
        pool[i].sock = make_socket();
        if (pool[i].sock == INVALID_SOCKET) { connects_fail++; continue; }
        if (!post_connect(iocp, &pool[i], &sa)) {
            connects_fail++;
            closesocket(pool[i].sock);
            pool[i].sock = INVALID_SOCKET;
        }
    }

    uint64_t start = now_us();
    uint64_t deadline = start + (uint64_t)duration_sec * 1000000;

    while (g_running && now_us() < deadline) {
        DWORD bytes = 0; ULONG_PTR key = 0; LPOVERLAPPED ov = NULL;
        BOOL ok = GetQueuedCompletionStatus(iocp, &bytes, &key, &ov, 10);
        if (!ov) continue;
        io_op_t* op = (io_op_t*)ov;
        conn_t* c = op->conn;
        if (op->kind != OP_CONNECT) continue;

        if (ok) connects_ok++;
        else    connects_fail++;

        closesocket(c->sock);
        c->sock = make_socket();
        if (c->sock == INVALID_SOCKET) { connects_fail++; continue; }
        if (!post_connect(iocp, c, &sa)) {
            connects_fail++;
            closesocket(c->sock);
            c->sock = INVALID_SOCKET;
        }
    }

    double elapsed_sec = (double)(now_us() - start) / 1000000.0;
    emit_connrate_json(elapsed_sec, concurrency, connects_ok, connects_fail);

    for (int i = 0; i < concurrency; i++) {
        if (pool[i].sock != INVALID_SOCKET) closesocket(pool[i].sock);
    }
    CloseHandle(iocp);
    free(pool);
}

/* ----- MEMORY (IOCP) ----- */

static void run_memory(int argc, char** argv) {
    int target_conns = 10000;
    int hold_sec     = 5;

    for (int i = 0; i < argc; i++) {
        if (strcmp(argv[i], "-n") == 0 && i + 1 < argc)
            target_conns = atoi(argv[++i]);
        else if (strcmp(argv[i], "-w") == 0 && i + 1 < argc)
            hold_sec = atoi(argv[++i]);
    }
    parse_host_port(argc, argv);

    HANDLE iocp = CreateIoCompletionPort(INVALID_HANDLE_VALUE, NULL, 0, 1);
    conn_t* conns = calloc((size_t)target_conns, sizeof(conn_t));
    int connected = 0, failed = 0, completed = 0;

    struct sockaddr_in sa;
    make_addr(&sa);

    sock_t probe = make_socket();
    load_connectex(probe);
    closesocket(probe);

    fprintf(stderr, "memory: connecting %d to %s:%d...\n",
            target_conns, g_host, g_port);

    for (int i = 0; i < target_conns; i++) {
        conns[i].sock = make_socket();
        if (conns[i].sock == INVALID_SOCKET) { failed++; completed++; continue; }
        if (!post_connect(iocp, &conns[i], &sa)) { failed++; completed++; }
    }

    while (completed < target_conns) {
        DWORD bytes = 0; ULONG_PTR key = 0; LPOVERLAPPED ov = NULL;
        BOOL ok = GetQueuedCompletionStatus(iocp, &bytes, &key, &ov, 2000);
        if (!ov) break;
        io_op_t* op = (io_op_t*)ov;
        if (op->kind == OP_CONNECT) {
            completed++;
            if (ok) {
                setsockopt(op->conn->sock, SOL_SOCKET,
                           SO_UPDATE_CONNECT_CONTEXT, NULL, 0);
                op->conn->connected = true;
                connected++;
            } else {
                failed++;
            }
        }
    }

    fprintf(stderr, "READY %d/%d (failed: %d)\n", connected, target_conns, failed);
    Sleep((DWORD)hold_sec * 1000);

    emit_memory_json(target_conns, connected, get_rss_kb());

    for (int i = 0; i < target_conns; i++) {
        if (conns[i].sock != INVALID_SOCKET) closesocket(conns[i].sock);
    }
    CloseHandle(iocp);
    free(conns);
}

static BOOL WINAPI win_ctrl_handler(DWORD type) {
    (void)type;
    g_running = false;
    return TRUE;
}

static void platform_init(void) {
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
    SetConsoleCtrlHandler(win_ctrl_handler, TRUE);
}

static void platform_cleanup(void) {
    WSACleanup();
}

#endif /* _WIN32 */

/* ========================================================================== */
/* MAIN (shared)                                                              */
/* ========================================================================== */

int main(int argc, char** argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: tcp-bench <mode> [options]\n");
        fprintf(stderr, "modes:\n");
        fprintf(stderr, "  throughput  -n conns -d sec -s payload -h host -p port\n");
        fprintf(stderr, "  connrate    -c concurrency -d sec -h host -p port\n");
        fprintf(stderr, "  memory      -n conns -w hold_sec -h host -p port\n");
        return 1;
    }

    platform_init();

    const char* mode = argv[1];
    int sub_argc = argc - 2;
    char** sub_argv = argv + 2;

    if (strcmp(mode, "throughput") == 0) {
        run_throughput(sub_argc, sub_argv);
    } else if (strcmp(mode, "connrate") == 0) {
        run_connrate(sub_argc, sub_argv);
    } else if (strcmp(mode, "memory") == 0) {
        run_memory(sub_argc, sub_argv);
    } else {
        fprintf(stderr, "unknown mode: %s\n", mode);
        platform_cleanup();
        return 1;
    }

    platform_cleanup();
    return 0;
}
