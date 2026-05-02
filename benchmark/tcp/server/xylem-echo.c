#include "xylem.h"
#include "xylem/xylem-tcp.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DEFAULT_PORT 9000

static void _on_read(xylem_tcp_conn_t* conn, void* data, size_t len) {
    xylem_tcp_send(conn, data, len);
}

int main(int argc, char** argv) {
    int port = DEFAULT_PORT;
    if (argc > 1) port = atoi(argv[1]);

    xylem_startup();

    xylem_loop_t* loop = xylem_loop_create();

    xylem_addr_t addr;
    xylem_addr_pton("0.0.0.0", (uint16_t)port, &addr);

    xylem_tcp_handler_t handler = {
        .on_read = _on_read,
    };

    xylem_tcp_opts_t opts = {0};
    opts.disable_mss_clamp = true;

    xylem_tcp_server_t* server = xylem_tcp_listen(loop, &addr, &handler, &opts);
    if (!server) {
        fprintf(stderr, "failed to listen on port %d\n", port);
        return 1;
    }

    fprintf(stderr, "xylem tcp echo server listening on 0.0.0.0:%d\n", port);
    xylem_loop_run(loop);

    xylem_loop_destroy(loop);
    xylem_cleanup();
    return 0;
}
