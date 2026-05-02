#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#define MAX_EVENTS    4096
#define LATENCY_SLOTS 1000000

static const char* g_host = "127.0.0.1";
static int         g_port = 9000;
static bool        g_running = true;

static uint64_t now_us(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000 + (uint64_t)ts.tv_nsec / 1000;
}

static void set_nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

static long get_rss_kb(void) {
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
}

static void sig_handler(int sig) {
    (void)sig;
    g_running = false;
}

static int cmp_u64(const void* a, const void* b) {
    uint64_t va = *(const uint64_t*)a;
    uint64_t vb = *(const uint64_t*)b;
    if (va < vb) return -1;
    if (va > vb) return 1;
    return 0;
}

/* ========================================================================== */
/* THROUGHPUT MODE                                                             */
/* ========================================================================== */

typedef struct {
    int      fd;
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
        else if (strcmp(argv[i], "-h") == 0 && i + 1 < argc)
            g_host = argv[++i];
        else if (strcmp(argv[i], "-p") == 0 && i + 1 < argc)
            g_port = atoi(argv[++i]);
    }

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

    int epfd = epoll_create1(0);

    struct sockaddr_in sa;
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port   = htons((uint16_t)g_port);
    inet_pton(AF_INET, g_host, &sa.sin_addr);

    fprintf(stderr, "connecting %d to %s:%d...\n", target_conns, g_host, g_port);

    int created = 0;
    while (created < target_conns) {
        int batch = 1000;
        if (created + batch > target_conns) batch = target_conns - created;
        for (int i = 0; i < batch; i++) {
            int fd = socket(AF_INET, SOCK_STREAM, 0);
            if (fd < 0) break;
            set_nonblocking(fd);
            int yes = 1;
            setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &yes, sizeof(yes));
            int rc = connect(fd, (struct sockaddr*)&sa, sizeof(sa));
            if (rc < 0 && errno != EINPROGRESS) { close(fd); break; }
            struct epoll_event ev = { .events = EPOLLOUT | EPOLLIN | EPOLLET, .data.fd = fd };
            epoll_ctl(epfd, EPOLL_CTL_ADD, fd, &ev);
            conns[fd].fd = fd;
            conns[fd].connected = false;
            conns[fd].awaiting_echo = false;
            created++;
        }
        struct epoll_event events[MAX_EVENTS];
        int nev = epoll_wait(epfd, events, MAX_EVENTS, 10);
        for (int i = 0; i < nev; i++) {
            conn_t* c = &conns[events[i].data.fd];
            if (!c->connected && (events[i].events & EPOLLOUT)) {
                int err = 0; socklen_t len = sizeof(err);
                getsockopt(c->fd, SOL_SOCKET, SO_ERROR, &err, &len);
                if (err == 0) { c->connected = true; connected++; }
                else failed++;
            }
        }
    }

    while (connected + failed < created) {
        struct epoll_event events[MAX_EVENTS];
        int nev = epoll_wait(epfd, events, MAX_EVENTS, 200);
        for (int i = 0; i < nev; i++) {
            conn_t* c = &conns[events[i].data.fd];
            if (!c->connected && (events[i].events & EPOLLOUT)) {
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
        struct epoll_event events[MAX_EVENTS];
        int nev = epoll_wait(epfd, events, MAX_EVENTS, 100);
        for (int i = 0; i < nev; i++) {
            if (!(events[i].events & EPOLLIN)) continue;
            conn_t* c = &conns[events[i].data.fd];
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

    uint64_t elapsed = now_us() - start;
    double elapsed_sec = (double)elapsed / 1000000.0;

    qsort(latencies, (size_t)lat_count, sizeof(uint64_t), cmp_u64);
    uint64_t p50 = lat_count > 0 ? latencies[lat_count / 2] : 0;
    uint64_t p99 = lat_count > 0 ? latencies[(int)(lat_count * 0.99)] : 0;
    uint64_t max = lat_count > 0 ? latencies[lat_count - 1] : 0;

    printf("{\n");
    printf("  \"connections\": %d,\n", connected);
    printf("  \"duration_sec\": %.2f,\n", elapsed_sec);
    printf("  \"messages_sent\": %lu,\n", msgs_sent);
    printf("  \"messages_recv\": %lu,\n", msgs_recv);
    printf("  \"throughput_msg_per_sec\": %.0f,\n", (double)msgs_recv / elapsed_sec);
    printf("  \"latency_p50_us\": %lu,\n", p50);
    printf("  \"latency_p99_us\": %lu,\n", p99);
    printf("  \"latency_max_us\": %lu,\n", max);
    printf("  \"memory_rss_kb\": %ld\n", get_rss_kb());
    printf("}\n");

    struct linger lg = { .l_onoff = 1, .l_linger = 0 };
    for (int fd = 0; fd < max_fds; fd++) {
        if (conns[fd].fd > 0) {
            setsockopt(fd, SOL_SOCKET, SO_LINGER, &lg, sizeof(lg));
            close(fd);
        }
    }
    close(epfd);
    free(conns);
    free(latencies);
}

/* ========================================================================== */
/* CONNRATE MODE                                                              */
/* ========================================================================== */

static void run_connrate(int argc, char** argv) {
    int concurrency  = 256;
    int duration_sec = 10;

    for (int i = 0; i < argc; i++) {
        if (strcmp(argv[i], "-c") == 0 && i + 1 < argc)
            concurrency = atoi(argv[++i]);
        else if (strcmp(argv[i], "-d") == 0 && i + 1 < argc)
            duration_sec = atoi(argv[++i]);
        else if (strcmp(argv[i], "-h") == 0 && i + 1 < argc)
            g_host = argv[++i];
        else if (strcmp(argv[i], "-p") == 0 && i + 1 < argc)
            g_port = atoi(argv[++i]);
    }

    int epfd = epoll_create1(0);
    int inflight = 0;
    uint64_t connects_ok = 0;
    uint64_t connects_fail = 0;

    struct sockaddr_in sa;
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port   = htons((uint16_t)g_port);
    inet_pton(AF_INET, g_host, &sa.sin_addr);

    fprintf(stderr, "connrate: %d concurrent to %s:%d for %ds\n",
            concurrency, g_host, g_port, duration_sec);

    for (int i = 0; i < concurrency; i++) {
        int fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
        if (fd < 0) break;
        int rc = connect(fd, (struct sockaddr*)&sa, sizeof(sa));
        if (rc < 0 && errno != EINPROGRESS) { close(fd); break; }
        struct epoll_event ev = { .events = EPOLLOUT | EPOLLET, .data.fd = fd };
        epoll_ctl(epfd, EPOLL_CTL_ADD, fd, &ev);
        inflight++;
    }

    uint64_t start = now_us();
    uint64_t deadline = start + (uint64_t)duration_sec * 1000000;
    struct epoll_event events[MAX_EVENTS];

    while (g_running && now_us() < deadline) {
        int nev = epoll_wait(epfd, events, MAX_EVENTS, 10);
        for (int i = 0; i < nev; i++) {
            int fd = events[i].data.fd;

            if (events[i].events & (EPOLLERR | EPOLLHUP)) {
                epoll_ctl(epfd, EPOLL_CTL_DEL, fd, NULL);
                close(fd);
                inflight--;
                connects_fail++;
                int nfd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
                if (nfd >= 0) {
                    int rc = connect(nfd, (struct sockaddr*)&sa, sizeof(sa));
                    if (rc < 0 && errno != EINPROGRESS) { close(nfd); continue; }
                    struct epoll_event ev = { .events = EPOLLOUT | EPOLLET, .data.fd = nfd };
                    epoll_ctl(epfd, EPOLL_CTL_ADD, nfd, &ev);
                    inflight++;
                }
                continue;
            }

            if (events[i].events & EPOLLOUT) {
                int err = 0; socklen_t len = sizeof(err);
                getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &len);
                epoll_ctl(epfd, EPOLL_CTL_DEL, fd, NULL);
                inflight--;

                if (err == 0) {
                    connects_ok++;
                    struct linger lg = { .l_onoff = 1, .l_linger = 0 };
                    setsockopt(fd, SOL_SOCKET, SO_LINGER, &lg, sizeof(lg));
                    close(fd);
                } else {
                    connects_fail++;
                    close(fd);
                }

                int nfd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
                if (nfd >= 0) {
                    int rc = connect(nfd, (struct sockaddr*)&sa, sizeof(sa));
                    if (rc < 0 && errno != EINPROGRESS) { close(nfd); continue; }
                    struct epoll_event ev = { .events = EPOLLOUT | EPOLLET, .data.fd = nfd };
                    epoll_ctl(epfd, EPOLL_CTL_ADD, nfd, &ev);
                    inflight++;
                }
            }
        }
    }

    uint64_t elapsed = now_us() - start;
    double elapsed_sec = (double)elapsed / 1000000.0;

    printf("{\n");
    printf("  \"benchmark\": \"connrate\",\n");
    printf("  \"duration_sec\": %.2f,\n", elapsed_sec);
    printf("  \"concurrency\": %d,\n", concurrency);
    printf("  \"total_connects\": %lu,\n", connects_ok);
    printf("  \"failed_connects\": %lu,\n", connects_fail);
    printf("  \"connects_per_sec\": %.0f\n", (double)connects_ok / elapsed_sec);
    printf("}\n");

    close(epfd);
}

/* ========================================================================== */
/* MEMORY MODE                                                                */
/* ========================================================================== */

static void run_memory(int argc, char** argv) {
    int target_conns = 10000;
    int hold_sec     = 5;

    for (int i = 0; i < argc; i++) {
        if (strcmp(argv[i], "-n") == 0 && i + 1 < argc)
            target_conns = atoi(argv[++i]);
        else if (strcmp(argv[i], "-w") == 0 && i + 1 < argc)
            hold_sec = atoi(argv[++i]);
        else if (strcmp(argv[i], "-h") == 0 && i + 1 < argc)
            g_host = argv[++i];
        else if (strcmp(argv[i], "-p") == 0 && i + 1 < argc)
            g_port = atoi(argv[++i]);
    }

    int epfd = epoll_create1(0);
    int connected = 0;
    int failed = 0;

    struct sockaddr_in sa;
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port   = htons((uint16_t)g_port);
    inet_pton(AF_INET, g_host, &sa.sin_addr);

    fprintf(stderr, "memory: connecting %d to %s:%d...\n",
            target_conns, g_host, g_port);

    int created = 0;
    while (created < target_conns) {
        int batch = 1000;
        if (created + batch > target_conns) batch = target_conns - created;
        for (int i = 0; i < batch; i++) {
            int fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
            if (fd < 0) break;
            int yes = 1;
            setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &yes, sizeof(yes));
            int rc = connect(fd, (struct sockaddr*)&sa, sizeof(sa));
            if (rc < 0 && errno != EINPROGRESS) { close(fd); break; }
            struct epoll_event ev = { .events = EPOLLOUT | EPOLLET, .data.fd = fd };
            epoll_ctl(epfd, EPOLL_CTL_ADD, fd, &ev);
            created++;
        }
        struct epoll_event events[MAX_EVENTS];
        int nev = epoll_wait(epfd, events, MAX_EVENTS, 10);
        for (int i = 0; i < nev; i++) {
            if (events[i].events & EPOLLOUT) {
                int err = 0; socklen_t len = sizeof(err);
                getsockopt(events[i].data.fd, SOL_SOCKET, SO_ERROR, &err, &len);
                if (err == 0) connected++;
                else failed++;
            }
        }
    }

    while (connected + failed < created) {
        struct epoll_event events[MAX_EVENTS];
        int nev = epoll_wait(epfd, events, MAX_EVENTS, 200);
        for (int i = 0; i < nev; i++) {
            if (events[i].events & EPOLLOUT) {
                int err = 0; socklen_t len = sizeof(err);
                getsockopt(events[i].data.fd, SOL_SOCKET, SO_ERROR, &err, &len);
                if (err == 0) connected++;
                else failed++;
            }
        }
    }

    fprintf(stderr, "READY %d/%d (failed: %d)\n", connected, target_conns, failed);
    sleep((unsigned)hold_sec);

    printf("{\n");
    printf("  \"benchmark\": \"memory\",\n");
    printf("  \"target_connections\": %d,\n", target_conns);
    printf("  \"established_connections\": %d,\n", connected);
    printf("  \"client_rss_kb\": %ld\n", get_rss_kb());
    printf("}\n");

    struct linger lg = { .l_onoff = 1, .l_linger = 0 };
    int max_fds = target_conns + 100;
    for (int fd = 3; fd < max_fds + 3; fd++) {
        setsockopt(fd, SOL_SOCKET, SO_LINGER, &lg, sizeof(lg));
        close(fd);
    }
    close(epfd);
}

/* ========================================================================== */
/* MAIN                                                                       */
/* ========================================================================== */

int main(int argc, char** argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: tcp-bench <mode> [options]\n");
        fprintf(stderr, "modes:\n");
        fprintf(stderr, "  throughput  -n conns -d sec -s payload -h host -p port\n");
        fprintf(stderr, "  connrate   -c concurrency -d sec -h host -p port\n");
        fprintf(stderr, "  memory     -n conns -w hold_sec -h host -p port\n");
        return 1;
    }

    signal(SIGINT, sig_handler);
    signal(SIGPIPE, SIG_IGN);

    struct rlimit rl = { .rlim_cur = 200000, .rlim_max = 200000 };
    setrlimit(RLIMIT_NOFILE, &rl);

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
        return 1;
    }

    return 0;
}
