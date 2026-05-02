#include <hv/hloop.h>
#include <stdio.h>
#include <stdlib.h>

static void on_read(hio_t* io, void* buf, int readbytes) {
    hio_write(io, buf, readbytes);
}

int main(int argc, char** argv) {
    int port = 9001;
    if (argc > 1) port = atoi(argv[1]);

    hloop_t* loop = hloop_new(0);

    hio_t* io = hloop_create_udp_server(loop, "0.0.0.0", port);
    if (!io) {
        fprintf(stderr, "failed to bind on port %d\n", port);
        return 1;
    }

    hio_setcb_read(io, on_read);
    hio_read(io);

    fprintf(stderr, "libhv udp echo server listening on 0.0.0.0:%d\n", port);
    hloop_run(loop);
    hloop_free(&loop);
    return 0;
}
