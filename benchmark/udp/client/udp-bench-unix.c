/* ==========================================================================
 * udp-bench: UDP load generator (POSIX).
 *
 *   Linux -> epoll   (readiness / reactor)
 *   macOS -> kqueue  (readiness / reactor)
 *
 * Modes: throughput | memory
 *
 * UDP is connectionless, so there is no "connrate" mode (cf. tcp-bench). Each
 * client socket is connect()ed to fix the peer address, which lets us use
 * plain read()/write() and lets the kernel deliver only datagrams from that
 * peer. Datagrams are message-framed: one recv == one echoed reply, so there
 * is no stream re-assembly (unlike the TCP client's recv_accum loop).
 *
 * The Windows client lives in a separate file (udp-bench-win.c) because it
 * uses IOCP (a completion / proactor model) whose control flow differs from
 * the readiness model used here. Shared concepts (timing, RSS sampling,
 * JSON/statistics emission, argument parsing) are reimplemented there rather
 * than #ifdef-merged into this file.
 * ========================================================================== */

#define _GNU_SOURCE

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <signal.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>

typedef int sock_t;
#define BENCH_INVALID_SOCK (-1)

#define MAX_EVENTS    4096
#define LATENCY_SLOTS 1000000

static const char*   g_host    = "127.0.0.1";
static int           g_port    = 9001;
static volatile bool g_running = true;

/* -------------------------------------------------------------------------- */
/* shared helpers: timing, memory, statistics, output                         */
/* -------------------------------------------------------------------------- */

static uint64_t now_us(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000 + (uint64_t)ts.tv_nsec / 1000;
}

static long get_rss_kb(void) {
#if defined(__linux__)
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
    bool     created;
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
    if (payload_size > 65507) payload_size = 65507; /* max UDP datagram */

    int max_fds = target_conns + 100;
    conn_t* conns = calloc((size_t)max_fds, sizeof(conn_t));
    uint64_t* latencies = malloc(LATENCY_SLOTS * sizeof(uint64_t));
    int lat_count = 0;
    int connected = 0;
    uint64_t msgs_sent = 0;
    uint64_t msgs_recv = 0;

    int epfd = poller_create();

    struct sockaddr_in sa;
    make_addr(&sa);

    fprintf(stderr, "creating %d udp sockets to %s:%d...\n",
            target_conns, g_host, g_port);

    for (int i = 0; i < target_conns; i++) {
        sock_t fd = socket(AF_INET, SOCK_DGRAM, 0);
        if (fd < 0) break;
        set_nonblocking(fd);
        /* connect() fixes the peer; subsequent read()/write() need no addr. */
        if (connect(fd, (struct sockaddr*)&sa, sizeof(sa)) < 0) {
            close(fd);
            break;
        }
        poller_add(epfd, fd, true, false);
        conns[fd].fd = fd;
        conns[fd].created = true;
        connected++;
    }

    fprintf(stderr, "created: %d / %d, starting echo benchmark for %ds...\n",
            connected, target_conns, duration_sec);

    char payload[65507];
    memset(payload, 'A', (size_t)payload_size);

    /* prime: one datagram per socket */
    for (int fd = 0; fd < max_fds; fd++) {
        if (conns[fd].created) {
            conns[fd].send_ts = now_us();
            ssize_t n = write(fd, payload, (size_t)payload_size);
            if (n == payload_size) msgs_sent++;
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
                if (n <= 0) break; /* EAGAIN drains the edge-triggered fd */
                uint64_t lat = now_us() - c->send_ts;
                if (lat_count < LATENCY_SLOTS) latencies[lat_count++] = lat;
                msgs_recv++;
                /* send the next datagram immediately (ping-pong) */
                c->send_ts = now_us();
                ssize_t w = write(c->fd, payload, (size_t)payload_size);
                if (w == payload_size) msgs_sent++;
            }
        }
    }

    double elapsed_sec = (double)(now_us() - start) / 1000000.0;
    emit_throughput_json(connected, elapsed_sec, msgs_sent, msgs_recv,
                         latencies, lat_count);

    for (int fd = 0; fd < max_fds; fd++) {
        if (conns[fd].created) close(fd);
    }
    close(epfd);
    free(conns);
    free(latencies);
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
    int created = 0;

    struct sockaddr_in sa;
    make_addr(&sa);

    fprintf(stderr, "memory: creating %d udp sockets to %s:%d...\n",
            target_conns, g_host, g_port);

    int max_fds = target_conns + 100;
    for (int i = 0; i < target_conns; i++) {
        sock_t fd = socket(AF_INET, SOCK_DGRAM, 0);
        if (fd < 0) break;
        set_nonblocking(fd);
        if (connect(fd, (struct sockaddr*)&sa, sizeof(sa)) < 0) {
            close(fd);
            break;
        }
        poller_add(epfd, fd, true, false);
        created++;
    }

    fprintf(stderr, "READY %d/%d\n", created, target_conns);
    sleep((unsigned)hold_sec);

    emit_memory_json(target_conns, created, get_rss_kb());

    for (int fd = 3; fd < max_fds + 3; fd++) close(fd);
    close(epfd);
}

static void platform_init(void) {
    signal(SIGINT, sig_handler);
    signal(SIGPIPE, SIG_IGN);
    struct rlimit rl = { .rlim_cur = 200000, .rlim_max = 200000 };
    setrlimit(RLIMIT_NOFILE, &rl);
}

static void platform_cleanup(void) {}

/* ========================================================================== */
/* MAIN                                                                       */
/* ========================================================================== */

int main(int argc, char** argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: udp-bench <mode> [options]\n");
        fprintf(stderr, "modes:\n");
        fprintf(stderr, "  throughput  -n conns -d sec -s payload -h host -p port\n");
        fprintf(stderr, "  memory      -n conns -w hold_sec -h host -p port\n");
        return 1;
    }

    platform_init();

    const char* mode = argv[1];
    int sub_argc = argc - 2;
    char** sub_argv = argv + 2;

    if (strcmp(mode, "throughput") == 0) {
        run_throughput(sub_argc, sub_argv);
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
