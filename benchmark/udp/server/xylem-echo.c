#include "xylem.h"
#include "runtime/loop.h"
#include "xylem/net/xylem-udp.h"

#include <stdio.h>
#include <stdlib.h>

#define DEFAULT_PORT 9001

static void _on_read(xylem_udp_t* udp, void* data, size_t len,
                     const char* host, uint16_t port) {
    xylem_udp_send(udp, host, port, data, len);
}

int main(int argc, char** argv) {
    int port = DEFAULT_PORT;
    if (argc > 1) port = atoi(argv[1]);


    loop_t* loop = loop_create();

    xylem_udp_handler_t handler = {
        .on_read = _on_read,
    };

    xylem_udp_t* udp = xylem_udp_listen(loop, "0.0.0.0", (uint16_t)port,
                                         &handler);
    if (!udp) {
        fprintf(stderr, "failed to bind on port %d\n", port);
        return 1;
    }

    fprintf(stderr, "xylem udp echo server listening on 0.0.0.0:%d\n", port);
    loop_run(loop);

    loop_destroy(loop);
    return 0;
}
