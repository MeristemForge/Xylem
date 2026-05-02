#include <hv/hloop.h>
#include <hv/hsocket.h>
#include <netinet/tcp.h>
#include <stdio.h>
#include <stdlib.h>

static void on_close(hio_t* io) {
    void* buf = hio_context(io);
    if (buf) free(buf);
}

static void on_read(hio_t* io, void* buf, int readbytes) {
    hio_write(io, buf, readbytes);
}

static void on_accept(hio_t* io) {
    int fd = hio_fd(io);
    int yes = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &yes, sizeof(yes));
    char* buf = (char*)malloc(65536);
    hio_set_context(io, buf);
    hio_set_readbuf(io, buf, 65536);
    hio_setcb_close(io, on_close);
    hio_setcb_read(io, on_read);
    hio_read(io);
}

int main(int argc, char** argv) {
    int port = 9000;
    if (argc > 1) port = atoi(argv[1]);

    hloop_t* loop = hloop_new(0);

    hio_t* listenio = hloop_create_tcp_server(loop, "0.0.0.0", port, on_accept);
    if (!listenio) {
        fprintf(stderr, "failed to listen on port %d\n", port);
        return 1;
    }

    fprintf(stderr, "libhv tcp echo server listening on 0.0.0.0:%d\n", port);
    hloop_run(loop);
    hloop_free(&loop);
    return 0;
}
