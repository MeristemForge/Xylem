#include "xylem.h"
#include "xylem/xylem-udp.h"

#include <stdio.h>
#include <stdlib.h>

#define DEFAULT_PORT 9001

static void _on_read(xylem_udp_t* udp, void* data, size_t len,
                     xylem_addr_t* addr) {
    xylem_udp_send(udp, addr, data, len);
}

int main(int argc, char** argv) {
    int port = DEFAULT_PORT;
    if (argc > 1) port = atoi(argv[1]);

    xylem_startup();

    xylem_loop_t* loop = xylem_loop_create();

    xylem_addr_t addr;
    xylem_addr_pton("0.0.0.0", (uint16_t)port, &addr);

    xylem_udp_handler_t handler = {
        .on_read = _on_read,
    };

    xylem_udp_t* udp = xylem_udp_listen(loop, &addr, &handler);
    if (!udp) {
        fprintf(stderr, "failed to bind on port %d\n", port);
        return 1;
    }

    fprintf(stderr, "xylem udp echo server listening on 0.0.0.0:%d\n", port);
    xylem_loop_run(loop);

    xylem_loop_destroy(loop);
    xylem_cleanup();
    return 0;
}
