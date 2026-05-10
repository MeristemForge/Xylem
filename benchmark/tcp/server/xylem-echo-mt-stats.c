/**
 * Same as xylem-echo-mt, but built against a libxylem compiled with
 * -DXYLEM_IOWAIT_STATS and dumps iowait wait/resume latencies when
 * it receives SIGUSR1 or SIGTERM. Used only by perf investigation;
 * not part of the regular benchmark matrix.
 */

#include "xylem.h"

/* Project-internal headers; only available when stats macros are
 * set. The benchmark build script wires this via -I and -D. */
#include "runtime/iowait.h"
#include "runtime/scheduler.h"

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void _dump(int sig) {
    char tag[32];
    snprintf(tag, sizeof(tag), "sig%d", sig);
#ifdef XYLEM_IOWAIT_STATS
    iowait_stats_dump(tag);
#endif
#ifdef XYLEM_SCHED_STATS
    scheduler_stats_dump(tag);
#endif
    if (sig == SIGTERM || sig == SIGINT) {
        xylem_shutdown();
    }
}

static void _handle_conn(void* arg) {
    xylem_tcp_conn_t* conn = (xylem_tcp_conn_t*)arg;
    char* buf = (char*)malloc(65536);
    if (!buf) { xylem_tcp_close(conn); return; }

    for (;;) {
        int64_t n = xylem_tcp_recv(conn, buf, 65536);
        if (n <= 0) break;
        if (xylem_tcp_send(conn, buf, (size_t)n) != 0) break;
    }

    free(buf);
    xylem_tcp_close(conn);
}

static void _acceptor(void* arg) {
    int port = *(int*)arg;

    xylem_tcp_opts_t opts = {0};
    opts.disable_mss_clamp = true;

    xylem_tcp_listener_t* server =
        xylem_tcp_listen("0.0.0.0", (uint16_t)port, &opts);
    if (!server) {
        fprintf(stderr, "failed to listen on port %d\n", port);
        xylem_shutdown();
        return;
    }

    fprintf(stderr, "xylem tcp echo server (stats) listening on 0.0.0.0:%d\n", port);

    for (;;) {
        xylem_tcp_conn_t* conn = xylem_tcp_accept(server);
        if (!conn) break;
        xylem_spawn(_handle_conn, conn);
    }
}

int main(int argc, char** argv) {
    int port    = 9000;
    int workers = 4;
    if (argc > 1) port    = atoi(argv[1]);
    if (argc > 2) workers = atoi(argv[2]);

    struct sigaction sa = {0};
    sa.sa_handler = _dump;
    sigaction(SIGUSR1, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT,  &sa, NULL);

#ifdef XYLEM_IOWAIT_STATS
    iowait_stats_reset();
#endif
#ifdef XYLEM_SCHED_STATS
    scheduler_stats_reset();
#endif

    xylem_opts_t rt_opts = {0};
    rt_opts.workers      = workers;
    xylem_run(_acceptor, &port, &rt_opts);

#ifdef XYLEM_IOWAIT_STATS
    iowait_stats_dump("final");
#endif
#ifdef XYLEM_SCHED_STATS
    scheduler_stats_dump("final");
#endif
    return 0;
}
