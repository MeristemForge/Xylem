/**
 * libhv multi-threaded TCP echo server.
 *
 * Model: N independent pthreads, each owns its own hloop_t and a
 * listen socket with SO_REUSEPORT. libhv exposes hloop_new_tcp_server
 * which accepts a pre-bound fd via hloop_create_tcp_server(host, port,
 * ...) that opens its own fd, so we bypass it and use hio_get / hio_add
 * on a socket we bound with REUSEPORT ourselves.
 *
 * Fairness: TCP_NODELAY on accepted sockets, backlog 4096, same 64 KB
 * read buffer as the single-threaded variant.
 */
#include <hv/hloop.h>
#include <hv/hsocket.h>

#include <netinet/in.h>
#include <netinet/tcp.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

typedef struct {
    int port;
    int idx;
} worker_t;

static int make_listen_socket(int port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    int yes = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
    setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &yes, sizeof(yes));
    struct sockaddr_in sa;
    memset(&sa, 0, sizeof(sa));
    sa.sin_family      = AF_INET;
    sa.sin_port        = htons((uint16_t)port);
    sa.sin_addr.s_addr = INADDR_ANY;
    if (bind(fd, (struct sockaddr*)&sa, sizeof(sa)) < 0) { close(fd); return -1; }
    if (listen(fd, 4096) < 0) { close(fd); return -1; }
    return fd;
}

static void on_close(hio_t* io) {
    void* buf = hio_context(io);
    if (buf) free(buf);
}

static void on_read(hio_t* io, void* buf, int n) {
    hio_write(io, buf, n);
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

static void* worker_main(void* arg) {
    worker_t* w = (worker_t*)arg;

    int fd = make_listen_socket(w->port);
    if (fd < 0) {
        fprintf(stderr, "libhv worker %d: listen failed\n", w->idx);
        return NULL;
    }

    hloop_t* loop = hloop_new(0);
    hio_t* listenio = haccept(loop, fd, on_accept);
    if (!listenio) {
        fprintf(stderr, "libhv worker %d: haccept failed\n", w->idx);
        hloop_free(&loop);
        close(fd);
        return NULL;
    }

    hloop_run(loop);
    hloop_free(&loop);
    return NULL;
}

int main(int argc, char** argv) {
    int port = 9000;
    int workers = 4;
    if (argc > 1) port = atoi(argv[1]);
    if (argc > 2) workers = atoi(argv[2]);

    fprintf(stderr, "libhv-mt tcp echo: port=%d workers=%d\n", port, workers);

    pthread_t* tids = calloc((size_t)workers, sizeof(pthread_t));
    worker_t*  ws   = calloc((size_t)workers, sizeof(worker_t));
    for (int i = 0; i < workers; i++) {
        ws[i].port = port;
        ws[i].idx  = i;
        pthread_create(&tids[i], NULL, worker_main, &ws[i]);
    }
    for (int i = 0; i < workers; i++) pthread_join(tids[i], NULL);
    free(tids);
    free(ws);
    return 0;
}
