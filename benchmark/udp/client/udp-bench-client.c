#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#define MAX_EVENTS     4096
#define PAYLOAD_SIZE   64
#define LATENCY_SLOTS  1000000

typedef struct {
    int      fd;
    uint64_t send_ts;
    bool     awaiting_echo;
} conn_t;

static int         g_target_conns  = 1000;
static int         g_duration_sec  = 30;
static const char* g_host          = "127.0.0.1";
static int         g_port          = 9001;
static int         g_epfd          = -1;
static conn_t*     g_conns         = NULL;
static int         g_connected     = 0;
static uint64_t    g_msgs_sent     = 0;
static uint64_t    g_msgs_recv     = 0;
static uint64_t*   g_latencies     = NULL;
static int         g_lat_count     = 0;
static bool        g_running       = true;

static uint64_t now_us(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000 + (uint64_t)ts.tv_nsec / 1000;
}

static void set_nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

static int create_udp_socket(struct sockaddr_in* sa, int idx) {
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) return -1;

    set_nonblocking(fd);

    if (connect(fd, (struct sockaddr*)sa, sizeof(*sa)) < 0) {
        close(fd);
        return -1;
    }

    struct epoll_event ev = {
        .events  = EPOLLIN | EPOLLET,
        .data.u32 = (uint32_t)idx,
    };
    epoll_ctl(g_epfd, EPOLL_CTL_ADD, fd, &ev);

    g_conns[idx].fd = fd;
    g_conns[idx].awaiting_echo = false;
    g_connected++;
    return fd;
}

static void send_echo(conn_t* c) {
    char buf[PAYLOAD_SIZE];
    memset(buf, 'A', PAYLOAD_SIZE);
    c->send_ts = now_us();
    ssize_t n = write(c->fd, buf, PAYLOAD_SIZE);
    if (n == PAYLOAD_SIZE) {
        c->awaiting_echo = true;
        g_msgs_sent++;
    }
}

static void handle_read(conn_t* c) {
    char buf[4096];
    for (;;) {
        ssize_t n = read(c->fd, buf, sizeof(buf));
        if (n <= 0) break;
        size_t remaining = (size_t)n;
        while (remaining >= PAYLOAD_SIZE) {
            uint64_t lat = now_us() - c->send_ts;
            if (g_lat_count < LATENCY_SLOTS) {
                g_latencies[g_lat_count++] = lat;
            }
            g_msgs_recv++;
            remaining -= PAYLOAD_SIZE;
        }
        c->awaiting_echo = false;
        send_echo(c);
    }
}

static int cmp_u64(const void* a, const void* b) {
    uint64_t va = *(const uint64_t*)a;
    uint64_t vb = *(const uint64_t*)b;
    if (va < vb) return -1;
    if (va > vb) return 1;
    return 0;
}

static void sig_handler(int sig) {
    (void)sig;
    g_running = false;
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

static void print_results(uint64_t elapsed_us) {
    double elapsed_sec = (double)elapsed_us / 1000000.0;

    qsort(g_latencies, (size_t)g_lat_count, sizeof(uint64_t), cmp_u64);
    uint64_t p50 = g_lat_count > 0 ? g_latencies[g_lat_count / 2] : 0;
    uint64_t p99 = g_lat_count > 0 ? g_latencies[(int)(g_lat_count * 0.99)] : 0;
    uint64_t max = g_lat_count > 0 ? g_latencies[g_lat_count - 1] : 0;

    printf("{\n");
    printf("  \"connections\": %d,\n", g_connected);
    printf("  \"duration_sec\": %.2f,\n", elapsed_sec);
    printf("  \"messages_sent\": %lu,\n", g_msgs_sent);
    printf("  \"messages_recv\": %lu,\n", g_msgs_recv);
    printf("  \"throughput_msg_per_sec\": %.0f,\n", (double)g_msgs_recv / elapsed_sec);
    printf("  \"latency_p50_us\": %lu,\n", p50);
    printf("  \"latency_p99_us\": %lu,\n", p99);
    printf("  \"latency_max_us\": %lu,\n", max);
    printf("  \"memory_rss_kb\": %ld\n", get_rss_kb());
    printf("}\n");
}

int main(int argc, char** argv) {
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-n") == 0 && i + 1 < argc)
            g_target_conns = atoi(argv[++i]);
        else if (strcmp(argv[i], "-d") == 0 && i + 1 < argc)
            g_duration_sec = atoi(argv[++i]);
        else if (strcmp(argv[i], "-h") == 0 && i + 1 < argc)
            g_host = argv[++i];
        else if (strcmp(argv[i], "-p") == 0 && i + 1 < argc)
            g_port = atoi(argv[++i]);
    }

    signal(SIGINT, sig_handler);
    signal(SIGPIPE, SIG_IGN);

    struct rlimit rl = { .rlim_cur = 200000, .rlim_max = 200000 };
    setrlimit(RLIMIT_NOFILE, &rl);

    g_conns = calloc((size_t)g_target_conns, sizeof(conn_t));
    g_latencies = malloc(LATENCY_SLOTS * sizeof(uint64_t));

    g_epfd = epoll_create1(0);

    struct sockaddr_in sa;
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port   = htons((uint16_t)g_port);
    inet_pton(AF_INET, g_host, &sa.sin_addr);

    fprintf(stderr, "creating %d udp sockets to %s:%d...\n",
            g_target_conns, g_host, g_port);

    for (int i = 0; i < g_target_conns; i++) {
        if (create_udp_socket(&sa, i) < 0) {
            fprintf(stderr, "failed at socket %d: %s\n", i, strerror(errno));
            break;
        }
    }

    fprintf(stderr, "created: %d / %d, starting echo benchmark for %ds...\n",
            g_connected, g_target_conns, g_duration_sec);

    for (int i = 0; i < g_connected; i++) {
        send_echo(&g_conns[i]);
    }

    uint64_t start = now_us();
    uint64_t deadline = start + (uint64_t)g_duration_sec * 1000000;

    while (g_running && now_us() < deadline) {
        struct epoll_event events[MAX_EVENTS];
        int nev = epoll_wait(g_epfd, events, MAX_EVENTS, 100);
        for (int i = 0; i < nev; i++) {
            conn_t* c = &g_conns[events[i].data.u32];
            if (events[i].events & EPOLLIN) {
                handle_read(c);
            }
        }
    }

    uint64_t elapsed = now_us() - start;
    print_results(elapsed);

    for (int i = 0; i < g_connected; i++) {
        close(g_conns[i].fd);
    }
    close(g_epfd);
    free(g_conns);
    free(g_latencies);
    return 0;
}
