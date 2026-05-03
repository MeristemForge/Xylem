#include "xylem.h"
#include "runtime/loop.h"

#include <stdio.h>
#include <stdlib.h>

static loop_t* g_loop;

static void _handle_conn(void* arg) {
    xylem_tcp_conn_t* conn = (xylem_tcp_conn_t*)arg;
    char* buf = (char*)malloc(65536);
    if (!buf) { xylem_tcp_close(conn); return; }

    for (;;) {
        ssize_t n = xylem_tcp_recv(conn, buf, 65536);
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

    xylem_tcp_listener_t* server = xylem_tcp_listen(g_loop, "0.0.0.0",
                                                   (uint16_t)port, &opts);
    if (!server) {
        fprintf(stderr, "failed to listen on port %d\n", port);
        loop_stop(g_loop);
        return;
    }

    fprintf(stderr, "xylem tcp echo server listening on 0.0.0.0:%d\n", port);

    for (;;) {
        xylem_tcp_conn_t* conn = xylem_tcp_accept(server);
        if (!conn) break;
        xylem_coro_spawn(g_loop, _handle_conn, conn);
    }
}

int main(int argc, char** argv) {
    int port = 9000;
    if (argc > 1) port = atoi(argv[1]);


    g_loop = loop_create();
    xylem_coro_spawn(g_loop, _acceptor, &port);
    loop_run(g_loop);

    loop_destroy(g_loop);
    return 0;
}
