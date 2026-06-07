/* Xylem UDP echo server (single-threaded).
 *
 * Coroutine model: xylem_udp_listen() binds the socket, then a single
 * coroutine loops xylem_udp_recv()/xylem_udp_send(), echoing each datagram
 * back to its sender. recv/send suspend the coroutine until the socket is
 * ready. Run under one scheduler worker (ST). */
#include "xylem.h"
#include "xylem/net/xylem-udp.h"

#include <stdio.h>
#include <stdlib.h>

#define DEFAULT_PORT 9001

static void _server(void* arg) {
    int port = *(int*)arg;

    xylem_udp_chan_t* udp = xylem_udp_listen("0.0.0.0", (uint16_t)port);
    if (!udp) {
        fprintf(stderr, "failed to bind on port %d\n", port);
        xylem_shutdown();
        return;
    }

    fprintf(stderr, "xylem udp echo server listening on 0.0.0.0:%d\n", port);

    char* buf = (char*)malloc(65536);
    if (!buf) {
        xylem_udp_close(udp);
        xylem_shutdown();
        return;
    }

    for (;;) {
        char     host[46];
        uint16_t peer_port = 0;
        int n = xylem_udp_recv(udp, buf, 65536, host, sizeof(host), &peer_port);
        if (n < 0) break;
        xylem_udp_send(udp, buf, n, host, peer_port);
    }

    free(buf);
    xylem_udp_close(udp);
}

int main(int argc, char** argv) {
    int port = DEFAULT_PORT;
    if (argc > 1) port = atoi(argv[1]);

    xylem_opts_t rt_opts = {0};
    rt_opts.workers = 1;
    xylem_run(_server, &port, &rt_opts);
    return 0;
}
