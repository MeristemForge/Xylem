#define _GNU_SOURCE
#include "xylem.h"
#include "runtime/loop.h"
#include "xylem/net/xylem-rudp.h"

#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define PAYLOAD_SIZE   64
#define LATENCY_SLOTS  1000000
#define MAX_CONNS      100000

typedef struct {
    xylem_rudp_conn_t* rudp;
    uint64_t           send_ts;
    bool               connected;
    bool               awaiting_echo;
} conn_t;

static int         g_target_conns  = 1000;
static int         g_duration_sec  = 30;
static const char* g_host          = "127.0.0.1";
static int         g_port          = 9002;
static loop_t* g_loop        = NULL;
static conn_t      g_conns[MAX_CONNS];
static int         g_connected     = 0;
static uint64_t    g_msgs_sent     = 0;
static uint64_t    g_msgs_recv     = 0;
static uint64_t*   g_latencies     = NULL;
static int         g_lat_count     = 0;
static bool        g_running       = true;
static uint64_t    g_start_time    = 0;

static uint64_t now_us(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000 + (uint64_t)ts.tv_nsec / 1000;
}

static int cmp_u64(const void* a, const void* b) {
    uint64_t va = *(const uint64_t*)a;
    uint64_t vb = *(const uint64_t*)b;
    if (va < vb) return -1;
    if (va > vb) return 1;
    return 0;
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

static void send_echo(conn_t* c) {
    char buf[PAYLOAD_SIZE];
    memset(buf, 'A', PAYLOAD_SIZE);
    c->send_ts = now_us();
    xylem_rudp_send(c->rudp, buf, PAYLOAD_SIZE);
    c->awaiting_echo = true;
    g_msgs_sent++;
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

static conn_t* find_conn_by_rudp(xylem_rudp_conn_t* rudp) {
    for (int i = 0; i < g_target_conns; i++) {
        if (g_conns[i].rudp == rudp) return &g_conns[i];
    }
    return NULL;
}

static void _on_connect(xylem_rudp_conn_t* rudp) {
    conn_t* c = find_conn_by_rudp(rudp);
    if (!c) return;
    c->connected = true;
    g_connected++;

    if (g_start_time > 0) {
        send_echo(c);
    }
}

static void _on_read(xylem_rudp_conn_t* rudp, void* data, size_t len) {
    (void)data;
    conn_t* c = find_conn_by_rudp(rudp);
    if (!c) return;

    size_t remaining = len;
    while (remaining >= PAYLOAD_SIZE) {
        uint64_t lat = now_us() - c->send_ts;
        if (g_lat_count < LATENCY_SLOTS) {
            g_latencies[g_lat_count++] = lat;
        }
        g_msgs_recv++;
        remaining -= PAYLOAD_SIZE;
    }
    c->awaiting_echo = false;

    uint64_t elapsed = now_us() - g_start_time;
    if (g_running && elapsed < (uint64_t)g_duration_sec * 1000000) {
        send_echo(c);
    }
}

static void _on_close(xylem_rudp_conn_t* rudp, int err, const char* errmsg) {
    (void)err;
    (void)errmsg;
    conn_t* c = find_conn_by_rudp(rudp);
    if (c) {
        c->connected = false;
        g_connected--;
    }
}

static void _on_timeout(loop_t* loop, xylem_timer_t* timer, void* ud) {
    (void)loop;
    (void)timer;
    (void)ud;
    g_running = false;

    uint64_t elapsed = now_us() - g_start_time;
    print_results(elapsed);

    for (int i = 0; i < g_target_conns; i++) {
        if (g_conns[i].rudp && g_conns[i].connected) {
            xylem_rudp_close(g_conns[i].rudp);
        }
    }

    loop_stop(g_loop);
}

static void sig_handler(int sig) {
    (void)sig;
    g_running = false;
    if (g_loop) loop_stop(g_loop);
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

    if (g_target_conns > MAX_CONNS) {
        fprintf(stderr, "max connections: %d\n", MAX_CONNS);
        return 1;
    }

    signal(SIGINT, sig_handler);
    signal(SIGPIPE, SIG_IGN);

    g_latencies = malloc(LATENCY_SLOTS * sizeof(uint64_t));
    memset(g_conns, 0, sizeof(g_conns));


    g_loop = loop_create();

    xylem_addr_t addr;
    xylem_addr_pton(g_host, (uint16_t)g_port, &addr);

    xylem_rudp_handler_t handler = {
        .on_connect = _on_connect,
        .on_read    = _on_read,
        .on_close   = _on_close,
    };

    xylem_rudp_opts_t opts = {0};

    fprintf(stderr, "connecting %d rudp to %s:%d...\n",
            g_target_conns, g_host, g_port);

    for (int i = 0; i < g_target_conns; i++) {
        xylem_rudp_conn_t* rudp = xylem_rudp_dial(g_loop, &addr, &handler, &opts);
        if (!rudp) {
            fprintf(stderr, "failed to dial rudp at connection %d\n", i);
            break;
        }
        g_conns[i].rudp = rudp;
    }

    /* wait for connections to establish, then start benchmark */
    g_start_time = now_us();

    xylem_timer_t* deadline = loop_create_timer(
        g_loop, (uint64_t)g_duration_sec * 1000, false, _on_timeout, NULL);
    (void)deadline;

    fprintf(stderr, "starting echo benchmark for %ds...\n", g_duration_sec);

    /* start echo on any already-connected sessions */
    for (int i = 0; i < g_target_conns; i++) {
        if (g_conns[i].connected) {
            send_echo(&g_conns[i]);
        }
    }

    loop_run(g_loop);

    if (g_running) {
        uint64_t elapsed = now_us() - g_start_time;
        print_results(elapsed);
    }

    loop_destroy(g_loop);
    free(g_latencies);
    return 0;
}
