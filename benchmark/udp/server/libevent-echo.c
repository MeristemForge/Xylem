#include <event2/event.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

static void read_cb(evutil_socket_t fd, short events, void* arg) {
    (void)events;
    (void)arg;
    char buf[65536];
    struct sockaddr_in peer;
    socklen_t peerlen = sizeof(peer);

    ssize_t n = recvfrom(fd, buf, sizeof(buf), 0,
                         (struct sockaddr*)&peer, &peerlen);
    if (n > 0) {
        sendto(fd, buf, (size_t)n, 0, (struct sockaddr*)&peer, peerlen);
    }
}

int main(int argc, char** argv) {
    int port = 9001;
    if (argc > 1) port = atoi(argv[1]);

    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) {
        perror("socket");
        return 1;
    }

    int yes = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family      = AF_INET;
    addr.sin_port        = htons((uint16_t)port);
    addr.sin_addr.s_addr = INADDR_ANY;

    if (bind(fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        perror("bind");
        close(fd);
        return 1;
    }

    struct event_base* base = event_base_new();
    struct event* ev = event_new(base, fd, EV_READ | EV_PERSIST, read_cb, NULL);
    event_add(ev, NULL);

    fprintf(stderr, "libevent udp echo server listening on 0.0.0.0:%d\n", port);
    event_base_dispatch(base);

    event_free(ev);
    event_base_free(base);
    close(fd);
    return 0;
}
