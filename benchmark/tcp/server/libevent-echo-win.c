#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>

#include <event2/buffer.h>
#include <event2/bufferevent.h>
#include <event2/event.h>
#include <event2/listener.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void read_cb(struct bufferevent* bev, void* ctx) {
    (void)ctx;
    struct evbuffer* input  = bufferevent_get_input(bev);
    struct evbuffer* output = bufferevent_get_output(bev);
    evbuffer_add_buffer(output, input);
}

static void event_cb(struct bufferevent* bev, short events, void* ctx) {
    (void)ctx;
    if (events & (BEV_EVENT_ERROR | BEV_EVENT_EOF)) {
        bufferevent_free(bev);
    }
}

static void accept_cb(struct evconnlistener* listener,
                      evutil_socket_t fd,
                      struct sockaddr* addr, int addrlen, void* ctx) {
    (void)addr;
    (void)addrlen;
    (void)ctx;

    int yes = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, (const char*)&yes, sizeof(yes));

    struct event_base* base = evconnlistener_get_base(listener);
    struct bufferevent* bev = bufferevent_socket_new(base, fd,
                                                     BEV_OPT_CLOSE_ON_FREE);
    bufferevent_set_max_single_read(bev, 65536);
    bufferevent_set_max_single_write(bev, 65536);
    bufferevent_setcb(bev, read_cb, NULL, event_cb, NULL);
    bufferevent_enable(bev, EV_READ | EV_WRITE);
}

int main(int argc, char** argv) {
    int port = 9000;
    if (argc > 1) port = atoi(argv[1]);

    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);

    struct event_base* base = event_base_new();

    struct sockaddr_in sin;
    memset(&sin, 0, sizeof(sin));
    sin.sin_family      = AF_INET;
    sin.sin_port        = htons((unsigned short)port);
    sin.sin_addr.s_addr = INADDR_ANY;

    struct evconnlistener* listener = evconnlistener_new_bind(
        base, accept_cb, NULL,
        LEV_OPT_REUSEABLE | LEV_OPT_CLOSE_ON_FREE,
        4096, (struct sockaddr*)&sin, sizeof(sin));

    if (!listener) {
        fprintf(stderr, "failed to listen on port %d\n", port);
        return 1;
    }

    fprintf(stderr, "libevent tcp echo server listening on 0.0.0.0:%d\n", port);
    event_base_dispatch(base);

    evconnlistener_free(listener);
    event_base_free(base);
    WSACleanup();
    return 0;
}
