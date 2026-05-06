/**
 * libuv multi-threaded TCP echo server.
 *
 * Model: N independent pthreads, each owns its own uv_loop_t and a
 * listen socket with SO_REUSEPORT. The kernel load-balances incoming
 * SYNs across the listen sockets (Linux >= 3.9). This is the nginx-
 * style pattern that libuv is idiomatically paired with for scaling
 * beyond one core -- libuv has no shared-scheduler cross-loop story.
 *
 * Fairness: TCP_NODELAY on accepted sockets, backlog 4096, same 64 KB
 * read buffer as the single-threaded variant.
 */
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>
#include <uv.h>

typedef struct {
    uv_write_t req;
    uv_buf_t   buf;
} write_req_t;

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

static void alloc_cb(uv_handle_t* h, size_t sz, uv_buf_t* buf) {
    (void)h;
    buf->base = malloc(sz);
    buf->len  = sz;
}

static void write_cb(uv_write_t* req, int status) {
    (void)status;
    write_req_t* wr = (write_req_t*)req;
    free(wr->buf.base);
    free(wr);
}

static void read_cb(uv_stream_t* s, ssize_t nread, const uv_buf_t* buf) {
    if (nread <= 0) {
        if (buf->base) free(buf->base);
        if (nread < 0) uv_close((uv_handle_t*)s, (uv_close_cb)free);
        return;
    }
    write_req_t* wr = malloc(sizeof(*wr));
    wr->buf = uv_buf_init(buf->base, (unsigned int)nread);
    uv_write(&wr->req, s, &wr->buf, 1, write_cb);
}

static void on_connection(uv_stream_t* server, int status) {
    if (status < 0) return;
    uv_tcp_t* c = malloc(sizeof(*c));
    uv_tcp_init(server->loop, c);
    if (uv_accept(server, (uv_stream_t*)c) == 0) {
        uv_tcp_nodelay(c, 1);
        uv_read_start((uv_stream_t*)c, alloc_cb, read_cb);
    } else {
        uv_close((uv_handle_t*)c, (uv_close_cb)free);
    }
}

static void* worker_main(void* arg) {
    worker_t* w = (worker_t*)arg;

    int fd = make_listen_socket(w->port);
    if (fd < 0) {
        fprintf(stderr, "libuv worker %d: listen failed\n", w->idx);
        return NULL;
    }

    uv_loop_t loop;
    uv_loop_init(&loop);

    uv_tcp_t listener;
    uv_tcp_init(&loop, &listener);
    uv_tcp_open(&listener, fd);
    uv_listen((uv_stream_t*)&listener, 4096, on_connection);

    uv_run(&loop, UV_RUN_DEFAULT);
    uv_loop_close(&loop);
    return NULL;
}

int main(int argc, char** argv) {
    int port = 9000;
    int workers = 4;
    if (argc > 1) port = atoi(argv[1]);
    if (argc > 2) workers = atoi(argv[2]);

    fprintf(stderr, "libuv-mt tcp echo: port=%d workers=%d\n", port, workers);

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
