#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <uv.h>

static uv_loop_t* loop;

typedef struct {
    uv_write_t req;
    uv_buf_t   buf;
} write_req_t;

static void alloc_cb(uv_handle_t* handle, size_t suggested_size,
                     uv_buf_t* buf) {
    (void)handle;
    buf->base = malloc(suggested_size);
    buf->len  = suggested_size;
}

static void write_cb(uv_write_t* req, int status) {
    (void)status;
    write_req_t* wr = (write_req_t*)req;
    free(wr->buf.base);
    free(wr);
}

static void read_cb(uv_stream_t* stream, ssize_t nread, const uv_buf_t* buf) {
    if (nread <= 0) {
        if (buf->base) free(buf->base);
        if (nread < 0) {
            uv_close((uv_handle_t*)stream, (uv_close_cb)free);
        }
        return;
    }

    write_req_t* wr = malloc(sizeof(write_req_t));
    wr->buf = uv_buf_init(buf->base, (unsigned int)nread);
    uv_write(&wr->req, stream, &wr->buf, 1, write_cb);
}

static void on_connection(uv_stream_t* server, int status) {
    if (status < 0) return;

    uv_tcp_t* client = malloc(sizeof(uv_tcp_t));
    uv_tcp_init(loop, client);

    if (uv_accept(server, (uv_stream_t*)client) == 0) {
        uv_tcp_nodelay(client, 1);
        uv_read_start((uv_stream_t*)client, alloc_cb, read_cb);
    } else {
        uv_close((uv_handle_t*)client, (uv_close_cb)free);
    }
}

int main(int argc, char** argv) {
    int port = 9000;
    if (argc > 1) port = atoi(argv[1]);

    loop = uv_default_loop();

    uv_tcp_t server;
    uv_tcp_init(loop, &server);

    struct sockaddr_in addr;
    uv_ip4_addr("0.0.0.0", port, &addr);
    uv_tcp_bind(&server, (const struct sockaddr*)&addr, 0);

    int r = uv_listen((uv_stream_t*)&server, 4096, on_connection);
    if (r) {
        fprintf(stderr, "listen error: %s\n", uv_strerror(r));
        return 1;
    }

    fprintf(stderr, "libuv tcp echo server listening on 0.0.0.0:%d\n", port);
    uv_run(loop, UV_RUN_DEFAULT);
    return 0;
}
