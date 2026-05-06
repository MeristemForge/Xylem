/**
 * libevent multi-threaded TCP echo server.
 *
 * Model: N independent pthreads, each owns its own event_base and a
 * listen socket with SO_REUSEPORT. Matches libevent's idiomatic
 * multi-core pattern (one base per thread, kernel LB).
 *
 * Fairness: TCP_NODELAY on accepted sockets, backlog 4096, same
 * bufferevent max read/write as the single-threaded variant.
 */
#include <event2/buffer.h>
#include <event2/bufferevent.h>
#include <event2/event.h>
#include <event2/listener.h>

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

static void read_cb(struct bufferevent* bev, void* ctx) {
    (void)ctx;
    evbuffer_add_buffer(bufferevent_get_output(bev), bufferevent_get_input(bev));
}

static void event_cb(struct bufferevent* bev, short what, void* ctx) {
    (void)ctx;
    if (what & (BEV_EVENT_ERROR | BEV_EVENT_EOF)) bufferevent_free(bev);
}

static void accept_cb(struct evconnlistener* l, evutil_socket_t fd,
                      struct sockaddr* sa, int salen, void* ctx) {
    (void)sa; (void)salen; (void)ctx;
    int yes = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &yes, sizeof(yes));
    struct event_base* base = evconnlistener_get_base(l);
    struct bufferevent* bev = bufferevent_socket_new(base, fd, BEV_OPT_CLOSE_ON_FREE);
    bufferevent_set_max_single_read(bev, 65536);
    bufferevent_set_max_single_write(bev, 65536);
    bufferevent_setcb(bev, read_cb, NULL, event_cb, NULL);
    bufferevent_enable(bev, EV_READ | EV_WRITE);
}

static void* worker_main(void* arg) {
    worker_t* w = (worker_t*)arg;

    int fd = make_listen_socket(w->port);
    if (fd < 0) {
        fprintf(stderr, "libevent worker %d: listen failed\n", w->idx);
        return NULL;
    }

    struct event_base* base = event_base_new();
    struct evconnlistener* listener = evconnlistener_new(
        base, accept_cb, NULL,
        LEV_OPT_CLOSE_ON_FREE, 4096, fd);
    if (!listener) {
        fprintf(stderr, "libevent worker %d: listener new failed\n", w->idx);
        event_base_free(base);
        return NULL;
    }

    event_base_dispatch(base);

    evconnlistener_free(listener);
    event_base_free(base);
    return NULL;
}

int main(int argc, char** argv) {
    int port = 9000;
    int workers = 4;
    if (argc > 1) port = atoi(argv[1]);
    if (argc > 2) workers = atoi(argv[2]);

    fprintf(stderr, "libevent-mt tcp echo: port=%d workers=%d\n", port, workers);

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
