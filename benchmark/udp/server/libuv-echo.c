#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <uv.h>

static uv_loop_t* loop_handle;
static uv_udp_t   server;

typedef struct {
    uv_udp_send_t req;
    uv_buf_t      buf;
} send_req_t;

static void alloc_cb(uv_handle_t* handle, size_t suggested_size,
                     uv_buf_t* buf) {
    (void)handle;
    buf->base = malloc(suggested_size);
    buf->len  = suggested_size;
}

static void send_cb(uv_udp_send_t* req, int status) {
    (void)status;
    send_req_t* sr = (send_req_t*)req;
    free(sr->buf.base);
    free(sr);
}

static void recv_cb(uv_udp_t* handle, ssize_t nread, const uv_buf_t* buf,
                    const struct sockaddr* addr, unsigned flags) {
    (void)flags;
    if (nread <= 0 || !addr) {
        if (buf->base) free(buf->base);
        return;
    }

    send_req_t* sr = malloc(sizeof(send_req_t));
    sr->buf = uv_buf_init(buf->base, (unsigned int)nread);
    uv_udp_send(&sr->req, handle, &sr->buf, 1, addr, send_cb);
}

int main(int argc, char** argv) {
    int port = 9001;
    if (argc > 1) port = atoi(argv[1]);

    loop_handle = uv_default_loop();

    uv_udp_init(loop_handle, &server);

    struct sockaddr_in addr;
    uv_ip4_addr("0.0.0.0", port, &addr);
    uv_udp_bind(&server, (const struct sockaddr*)&addr, UV_UDP_REUSEADDR);

    uv_udp_recv_start(&server, alloc_cb, recv_cb);

    fprintf(stderr, "libuv udp echo server listening on 0.0.0.0:%d\n", port);
    uv_run(loop_handle, UV_RUN_DEFAULT);
    return 0;
}
