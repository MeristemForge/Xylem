/* ==========================================================================
 * tls-bench: cross-platform TLS load generator (OpenSSL).
 *
 *   Linux   -> epoll   (readiness / reactor), OpenSSL over the socket fd
 *   macOS    -> kqueue  (readiness / reactor), OpenSSL over the socket fd
 *   Windows  -> IOCP    (completion / proactor), OpenSSL over memory BIOs
 *
 * Modes: throughput | connrate | memory
 *
 * TLS runs over TCP. The readiness backends (epoll/kqueue) let OpenSSL drive
 * the socket directly: a non-blocking connect() then SSL_connect cycling on
 * WANT_READ / WANT_WRITE. The IOCP backend cannot let OpenSSL touch the
 * socket, so it bridges via memory BIOs:
 *
 *   network -> WSARecv completion -> BIO_write(rbio)   (feed ciphertext in)
 *   SSL_*   -> BIO_read(wbio)      -> WSASend           (push ciphertext out)
 *
 * "connrate" measures full TLS handshakes per second (TCP connect + handshake),
 * the metric that stresses a TLS server's accept path. Certificates are not
 * verified (SSL_VERIFY_NONE): the bench servers use self-signed certs and we
 * are measuring throughput, not the PKI.
 *
 * Only the event backend and the per-mode run_* loops are split behind
 * #ifdef _WIN32; timing wrappers, RSS sampling, JSON emission, SSL context
 * setup, argument parsing and dispatch are shared.
 * ========================================================================== */

#ifdef _WIN32

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <mswsock.h>
#include <windows.h>
#include <psapi.h>

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "mswsock.lib")
#pragma comment(lib, "psapi.lib")

#else /* POSIX */

#define _GNU_SOURCE

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <signal.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#endif

#include <openssl/err.h>
#include <openssl/ssl.h>

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>

#ifdef _WIN32
typedef SOCKET sock_t;
#define BENCH_INVALID_SOCK INVALID_SOCKET
#else
typedef int sock_t;
#define BENCH_INVALID_SOCK (-1)
#endif

#define MAX_EVENTS    4096
#define LATENCY_SLOTS 1000000

static const char*   g_host    = "127.0.0.1";
static int           g_port    = 9443;
static volatile bool g_running = true;
static SSL_CTX*      g_ssl_ctx = NULL;

/* -------------------------------------------------------------------------- */
/* shared helpers: timing, memory, statistics, output                         */
/* -------------------------------------------------------------------------- */

static uint64_t now_us(void) {
#ifdef _WIN32
    static LARGE_INTEGER freq;
    if (freq.QuadPart == 0) {
        QueryPerformanceFrequency(&freq);
    }
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return (uint64_t)(c.QuadPart * 1000000ULL / (uint64_t)freq.QuadPart);
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000 + (uint64_t)ts.tv_nsec / 1000;
#endif
}

static long get_rss_kb(void) {
#if defined(_WIN32)
    PROCESS_MEMORY_COUNTERS pmc;
    if (GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc))) {
        return (long)(pmc.WorkingSetSize / 1024);
    }
    return 0;
#elif defined(__linux__)
    FILE* f = fopen("/proc/self/status", "r");
    if (!f) return 0;
    char line[256];
    long rss = 0;
    while (fgets(line, sizeof(line), f)) {
        if (strncmp(line, "VmRSS:", 6) == 0) {
            sscanf(line + 6, "%ld", &rss);
            break;
        }
    }
    fclose(f);
    return rss;
#else
    struct rusage ru;
    if (getrusage(RUSAGE_SELF, &ru) != 0) return 0;
#if defined(__APPLE__)
    return ru.ru_maxrss / 1024;
#else
    return ru.ru_maxrss;
#endif
#endif
}

static int cmp_u64(const void* a, const void* b) {
    uint64_t va = *(const uint64_t*)a;
    uint64_t vb = *(const uint64_t*)b;
    if (va < vb) return -1;
    if (va > vb) return 1;
    return 0;
}

static void emit_throughput_json(
    int connected, double elapsed_sec,
    uint64_t msgs_sent, uint64_t msgs_recv,
    uint64_t* latencies, int lat_count) {
    qsort(latencies, (size_t)lat_count, sizeof(uint64_t), cmp_u64);
    uint64_t p50 = lat_count > 0 ? latencies[lat_count / 2] : 0;
    uint64_t p99 = lat_count > 0 ? latencies[(int)(lat_count * 0.99)] : 0;
    uint64_t mx  = lat_count > 0 ? latencies[lat_count - 1] : 0;

    printf("{\n");
    printf("  \"connections\": %d,\n", connected);
    printf("  \"duration_sec\": %.2f,\n", elapsed_sec);
    printf("  \"messages_sent\": %" PRIu64 ",\n", msgs_sent);
    printf("  \"messages_recv\": %" PRIu64 ",\n", msgs_recv);
    printf("  \"throughput_msg_per_sec\": %.0f,\n",
           elapsed_sec > 0 ? (double)msgs_recv / elapsed_sec : 0.0);
    printf("  \"latency_p50_us\": %" PRIu64 ",\n", p50);
    printf("  \"latency_p99_us\": %" PRIu64 ",\n", p99);
    printf("  \"latency_max_us\": %" PRIu64 ",\n", mx);
    printf("  \"memory_rss_kb\": %ld\n", get_rss_kb());
    printf("}\n");
}

static void emit_connrate_json(
    double elapsed_sec, int concurrency,
    uint64_t connects_ok, uint64_t connects_fail) {
    printf("{\n");
    printf("  \"benchmark\": \"connrate\",\n");
    printf("  \"duration_sec\": %.2f,\n", elapsed_sec);
    printf("  \"concurrency\": %d,\n", concurrency);
    printf("  \"total_connects\": %" PRIu64 ",\n", connects_ok);
    printf("  \"failed_connects\": %" PRIu64 ",\n", connects_fail);
    printf("  \"connects_per_sec\": %.0f\n",
           elapsed_sec > 0 ? (double)connects_ok / elapsed_sec : 0.0);
    printf("}\n");
}

static void emit_memory_json(
    int target_conns, int established, long client_rss_kb) {
    printf("{\n");
    printf("  \"benchmark\": \"memory\",\n");
    printf("  \"target_connections\": %d,\n", target_conns);
    printf("  \"established_connections\": %d,\n", established);
    printf("  \"client_rss_kb\": %ld\n", client_rss_kb);
    printf("}\n");
}

/* parse common -h/-p; per-mode opts parsed by callers */
static void parse_host_port(int argc, char** argv) {
    for (int i = 0; i < argc; i++) {
        if (strcmp(argv[i], "-h") == 0 && i + 1 < argc)
            g_host = argv[++i];
        else if (strcmp(argv[i], "-p") == 0 && i + 1 < argc)
            g_port = atoi(argv[++i]);
    }
}

static void make_addr(struct sockaddr_in* sa) {
    memset(sa, 0, sizeof(*sa));
    sa->sin_family = AF_INET;
    sa->sin_port   = htons((uint16_t)g_port);
    inet_pton(AF_INET, g_host, &sa->sin_addr);
}

static void ssl_ctx_init(void) {
    SSL_library_init();
    SSL_load_error_strings();
    g_ssl_ctx = SSL_CTX_new(TLS_client_method());
    SSL_CTX_set_verify(g_ssl_ctx, SSL_VERIFY_NONE, NULL);
}

#ifdef _WIN32

/* ==========================================================================
 * IOCP backend (completion / proactor model) + OpenSSL memory BIOs.
 * ========================================================================== */

#define NET_BUF 65536
#define OUT_BUF 131072

typedef enum { OP_CONNECT, OP_RECV, OP_SEND } op_kind_t;

typedef enum {
    STATE_HANDSHAKING,
    STATE_READY,
    STATE_DEAD,
} conn_state_t;

typedef struct conn_s conn_t;

typedef struct {
    OVERLAPPED ov;       /* must be first */
    op_kind_t  kind;
    conn_t*    conn;
} io_op_t;

struct conn_s {
    sock_t       sock;
    SSL*         ssl;
    BIO*         rbio;   /* network -> SSL */
    BIO*         wbio;   /* SSL -> network */
    conn_state_t state;

    io_op_t  connect_op;
    io_op_t  recv_op;
    io_op_t  send_op;

    char     net_in[NET_BUF];   /* WSARecv target (ciphertext from peer)  */
    char     out_buf[OUT_BUF];  /* ciphertext queued for WSASend          */
    size_t   out_len;           /* total bytes queued                     */
    size_t   out_sent;          /* bytes acknowledged sent                */
    bool     sending;           /* a WSASend is in flight                 */

    WSABUF   wsa_recv;
    WSABUF   wsa_send;

    int      payload_size;
    uint64_t send_ts;
};

static LPFN_CONNECTEX g_ConnectEx = NULL;

static void load_connectex(sock_t s) {
    GUID guid = WSAID_CONNECTEX;
    DWORD bytes = 0;
    WSAIoctl(s, SIO_GET_EXTENSION_FUNCTION_POINTER,
             &guid, sizeof(guid),
             &g_ConnectEx, sizeof(g_ConnectEx),
             &bytes, NULL, NULL);
}

static sock_t make_socket(void) {
    sock_t s = WSASocketW(AF_INET, SOCK_STREAM, IPPROTO_TCP,
                          NULL, 0, WSA_FLAG_OVERLAPPED);
    if (s != INVALID_SOCKET) {
        int yes = 1;
        setsockopt(s, IPPROTO_TCP, TCP_NODELAY,
                   (const char*)&yes, sizeof(yes));
    }
    return s;
}

static bool bind_any(sock_t s) {
    struct sockaddr_in local;
    memset(&local, 0, sizeof(local));
    local.sin_family = AF_INET;
    local.sin_addr.s_addr = INADDR_ANY;
    local.sin_port = 0;
    return bind(s, (struct sockaddr*)&local, sizeof(local)) == 0;
}

static bool post_recv(conn_t* c) {
    memset(&c->recv_op.ov, 0, sizeof(OVERLAPPED));
    c->recv_op.kind = OP_RECV;
    c->recv_op.conn = c;
    c->wsa_recv.buf = c->net_in;
    c->wsa_recv.len = sizeof(c->net_in);
    DWORD flags = 0, recvd = 0;
    int rc = WSARecv(c->sock, &c->wsa_recv, 1, &recvd, &flags,
                     &c->recv_op.ov, NULL);
    if (rc == SOCKET_ERROR && WSAGetLastError() != WSA_IO_PENDING)
        return false;
    return true;
}

/* Post a WSASend for the unsent tail of out_buf, if idle and data pending. */
static bool maybe_send(conn_t* c) {
    if (c->sending) return true;
    if (c->out_sent >= c->out_len) return true; /* nothing queued */
    memset(&c->send_op.ov, 0, sizeof(OVERLAPPED));
    c->send_op.kind = OP_SEND;
    c->send_op.conn = c;
    c->wsa_send.buf = c->out_buf + c->out_sent;
    c->wsa_send.len = (ULONG)(c->out_len - c->out_sent);
    DWORD sent = 0;
    int rc = WSASend(c->sock, &c->wsa_send, 1, &sent, 0,
                     &c->send_op.ov, NULL);
    if (rc == SOCKET_ERROR && WSAGetLastError() != WSA_IO_PENDING)
        return false;
    c->sending = true;
    return true;
}

/* Drain ciphertext OpenSSL wants to transmit (wbio) into out_buf, then send. */
static bool flush_wbio(conn_t* c) {
    int pending;
    while ((pending = BIO_pending(c->wbio)) > 0) {
        if (c->out_len >= sizeof(c->out_buf)) break; /* buffer full; backpressure */
        size_t room = sizeof(c->out_buf) - c->out_len;
        int want = (pending < (int)room) ? pending : (int)room;
        int n = BIO_read(c->wbio, c->out_buf + c->out_len, want);
        if (n <= 0) break;
        c->out_len += (size_t)n;
    }
    return maybe_send(c);
}

/* Reset the send buffer once fully flushed (called on send completion). */
static void send_compact(conn_t* c) {
    if (c->out_sent >= c->out_len) {
        c->out_len = 0;
        c->out_sent = 0;
    }
}

static void conn_free(conn_t* c) {
    if (c->ssl) { SSL_free(c->ssl); c->ssl = NULL; c->rbio = NULL; c->wbio = NULL; }
    if (c->sock != INVALID_SOCKET) { closesocket(c->sock); c->sock = INVALID_SOCKET; }
    c->state = STATE_DEAD;
}

static bool post_connect(HANDLE iocp, conn_t* c, struct sockaddr_in* sa) {
    if (!bind_any(c->sock)) return false;
    if (!CreateIoCompletionPort((HANDLE)c->sock, iocp, (ULONG_PTR)c, 0))
        return false;
    memset(&c->connect_op.ov, 0, sizeof(OVERLAPPED));
    c->connect_op.kind = OP_CONNECT;
    c->connect_op.conn = c;
    DWORD sent = 0;
    BOOL ok = g_ConnectEx(c->sock, (struct sockaddr*)sa, sizeof(*sa),
                          NULL, 0, &sent, &c->connect_op.ov);
    if (!ok && WSAGetLastError() != ERROR_IO_PENDING) return false;
    return true;
}

/* TCP connected -> set up SSL + memory BIOs and kick the handshake. */
static bool conn_start_tls(conn_t* c) {
    setsockopt(c->sock, SOL_SOCKET, SO_UPDATE_CONNECT_CONTEXT, NULL, 0);
    c->ssl  = SSL_new(g_ssl_ctx);
    c->rbio = BIO_new(BIO_s_mem());
    c->wbio = BIO_new(BIO_s_mem());
    if (!c->ssl || !c->rbio || !c->wbio) return false;
    SSL_set_bio(c->ssl, c->rbio, c->wbio); /* SSL owns the BIOs now */
    SSL_set_connect_state(c->ssl);
    c->state = STATE_HANDSHAKING;
    if (!post_recv(c)) return false;
    SSL_do_handshake(c->ssl);        /* generates ClientHello into wbio */
    return flush_wbio(c);
}

/* Advance the handshake after new ciphertext arrived in rbio.
 * Returns 1 still handshaking, 0 just completed, -1 fatal. */
static int conn_pump_handshake(conn_t* c) {
    int rc = SSL_do_handshake(c->ssl);
    if (!flush_wbio(c)) return -1;
    if (rc == 1) { c->state = STATE_READY; return 0; }
    int err = SSL_get_error(c->ssl, rc);
    if (err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE) return 1;
    return -1;
}

/* For throughput: write one plaintext payload, push ciphertext out. */
static bool conn_app_write(conn_t* c, const char* payload) {
    c->send_ts = now_us();
    int n = SSL_write(c->ssl, payload, c->payload_size);
    if (n <= 0) return false;
    return flush_wbio(c);
}

/* ----- THROUGHPUT (IOCP) ----- */

static void run_throughput(int argc, char** argv) {
    int target_conns = 1000;
    int duration_sec = 30;
    int payload_size = 64;

    for (int i = 0; i < argc; i++) {
        if (strcmp(argv[i], "-n") == 0 && i + 1 < argc)
            target_conns = atoi(argv[++i]);
        else if (strcmp(argv[i], "-d") == 0 && i + 1 < argc)
            duration_sec = atoi(argv[++i]);
        else if (strcmp(argv[i], "-s") == 0 && i + 1 < argc)
            payload_size = atoi(argv[++i]);
    }
    parse_host_port(argc, argv);

    if (payload_size < 1) payload_size = 1;
    if (payload_size > 65536) payload_size = 65536;

    HANDLE iocp = CreateIoCompletionPort(INVALID_HANDLE_VALUE, NULL, 0, 1);
    conn_t* conns = calloc((size_t)target_conns, sizeof(conn_t));
    uint64_t* latencies = malloc(LATENCY_SLOTS * sizeof(uint64_t));
    int lat_count = 0;
    int connected = 0, ready = 0, completed_conn = 0;
    uint64_t msgs_sent = 0, msgs_recv = 0;

    char payload[65536];
    memset(payload, 'A', (size_t)payload_size);

    struct sockaddr_in sa;
    make_addr(&sa);

    sock_t probe = make_socket();
    load_connectex(probe);
    closesocket(probe);

    fprintf(stderr, "connecting %d tls to %s:%d...\n",
            target_conns, g_host, g_port);

    for (int i = 0; i < target_conns; i++) {
        conns[i].sock = make_socket();
        conns[i].payload_size = payload_size;
        if (conns[i].sock == INVALID_SOCKET) { completed_conn++; continue; }
        if (!post_connect(iocp, &conns[i], &sa)) {
            conn_free(&conns[i]);
            completed_conn++;
        }
    }

    /* establish phase: connect + TLS handshake for all sockets */
    uint64_t hs_deadline = now_us() + 30ULL * 1000000;
    while (ready + (completed_conn - connected) < target_conns &&
           now_us() < hs_deadline) {
        DWORD bytes = 0; ULONG_PTR key = 0; LPOVERLAPPED ov = NULL;
        BOOL ok = GetQueuedCompletionStatus(iocp, &bytes, &key, &ov, 1000);
        if (!ov) break;
        io_op_t* op = (io_op_t*)ov;
        conn_t* c = op->conn;

        if (op->kind == OP_CONNECT) {
            completed_conn++;
            if (ok && conn_start_tls(c)) connected++;
            else conn_free(c);
        } else if (op->kind == OP_RECV) {
            if (!ok || bytes == 0) { conn_free(c); continue; }
            BIO_write(c->rbio, c->net_in, (int)bytes);
            post_recv(c);
            if (c->state == STATE_HANDSHAKING) {
                int r = conn_pump_handshake(c);
                if (r == 0) ready++;
                else if (r < 0) conn_free(c);
            }
        } else if (op->kind == OP_SEND) {
            if (!ok) { conn_free(c); continue; }
            c->out_sent += bytes;
            c->sending = false;
            send_compact(c);
            maybe_send(c);
        }
    }

    fprintf(stderr, "tls connected: %d / %d, starting echo benchmark for %ds...\n",
            ready, target_conns, duration_sec);

    /* prime: one app write per ready connection */
    for (int i = 0; i < target_conns; i++) {
        if (conns[i].state == STATE_READY) {
            if (conn_app_write(&conns[i], payload)) msgs_sent++;
        }
    }

    uint64_t start = now_us();
    uint64_t deadline = start + (uint64_t)duration_sec * 1000000;

    while (g_running && now_us() < deadline) {
        DWORD bytes = 0; ULONG_PTR key = 0; LPOVERLAPPED ov = NULL;
        BOOL ok = GetQueuedCompletionStatus(iocp, &bytes, &key, &ov, 100);
        if (!ov) continue;
        io_op_t* op = (io_op_t*)ov;
        conn_t* c = op->conn;

        if (op->kind == OP_SEND) {
            if (!ok) { conn_free(c); continue; }
            c->out_sent += bytes;
            c->sending = false;
            send_compact(c);
            maybe_send(c);
            continue;
        }

        if (op->kind != OP_RECV) continue;
        if (!ok || bytes == 0) { conn_free(c); continue; }

        BIO_write(c->rbio, c->net_in, (int)bytes);
        post_recv(c);
        if (c->state != STATE_READY) continue;

        char buf[65536];
        for (;;) {
            int n = SSL_read(c->ssl, buf, (int)sizeof(buf));
            if (n <= 0) break;
            uint64_t lat = now_us() - c->send_ts;
            if (lat_count < LATENCY_SLOTS) latencies[lat_count++] = lat;
            msgs_recv++;
            if (conn_app_write(c, payload)) msgs_sent++;
        }
    }

    double elapsed_sec = (double)(now_us() - start) / 1000000.0;
    emit_throughput_json(ready, elapsed_sec, msgs_sent, msgs_recv,
                         latencies, lat_count);

    for (int i = 0; i < target_conns; i++) {
        if (conns[i].state != STATE_DEAD) conn_free(&conns[i]);
    }
    CloseHandle(iocp);
    free(conns);
    free(latencies);
}

/* ----- CONNRATE (IOCP): full TLS handshakes per second ----- */

static void run_connrate(int argc, char** argv) {
    int concurrency  = 256;
    int duration_sec = 10;

    for (int i = 0; i < argc; i++) {
        if (strcmp(argv[i], "-c") == 0 && i + 1 < argc)
            concurrency = atoi(argv[++i]);
        else if (strcmp(argv[i], "-d") == 0 && i + 1 < argc)
            duration_sec = atoi(argv[++i]);
    }
    parse_host_port(argc, argv);

    HANDLE iocp = CreateIoCompletionPort(INVALID_HANDLE_VALUE, NULL, 0, 1);
    conn_t* pool = calloc((size_t)concurrency, sizeof(conn_t));
    uint64_t connects_ok = 0, connects_fail = 0;

    struct sockaddr_in sa;
    make_addr(&sa);

    sock_t probe = make_socket();
    load_connectex(probe);
    closesocket(probe);

    fprintf(stderr, "connrate: %d concurrent tls handshakes to %s:%d for %ds\n",
            concurrency, g_host, g_port, duration_sec);

    for (int i = 0; i < concurrency; i++) {
        pool[i].sock = make_socket();
        if (pool[i].sock == INVALID_SOCKET) { connects_fail++; continue; }
        if (!post_connect(iocp, &pool[i], &sa)) {
            connects_fail++;
            conn_free(&pool[i]);
        }
    }

    uint64_t start = now_us();
    uint64_t deadline = start + (uint64_t)duration_sec * 1000000;

    while (g_running && now_us() < deadline) {
        DWORD bytes = 0; ULONG_PTR key = 0; LPOVERLAPPED ov = NULL;
        BOOL ok = GetQueuedCompletionStatus(iocp, &bytes, &key, &ov, 10);
        if (!ov) continue;
        io_op_t* op = (io_op_t*)ov;
        conn_t* c = op->conn;
        int done = 0; /* 1 ok, -1 fail */

        if (op->kind == OP_CONNECT) {
            if (ok && conn_start_tls(c)) { /* handshake in flight */ }
            else done = -1;
        } else if (op->kind == OP_RECV) {
            if (!ok || bytes == 0) done = -1;
            else {
                BIO_write(c->rbio, c->net_in, (int)bytes);
                post_recv(c);
                int r = conn_pump_handshake(c);
                if (r == 0) done = 1;
                else if (r < 0) done = -1;
            }
        } else if (op->kind == OP_SEND) {
            if (!ok) done = -1;
            else {
                c->out_sent += bytes;
                c->sending = false;
                send_compact(c);
                maybe_send(c);
            }
        }

        if (done != 0) {
            if (done > 0) connects_ok++; else connects_fail++;
            conn_free(c);
            /* recycle: start a fresh handshake to keep pressure up */
            c->sock = make_socket();
            c->state = STATE_HANDSHAKING;
            c->out_len = c->out_sent = 0;
            c->sending = false;
            if (c->sock == INVALID_SOCKET || !post_connect(iocp, c, &sa)) {
                connects_fail++;
                conn_free(c);
            }
        }
    }

    double elapsed_sec = (double)(now_us() - start) / 1000000.0;
    emit_connrate_json(elapsed_sec, concurrency, connects_ok, connects_fail);

    for (int i = 0; i < concurrency; i++) {
        if (pool[i].state != STATE_DEAD) conn_free(&pool[i]);
    }
    CloseHandle(iocp);
    free(pool);
}

/* ----- MEMORY (IOCP): hold N established TLS connections ----- */

static void run_memory(int argc, char** argv) {
    int target_conns = 10000;
    int hold_sec     = 5;

    for (int i = 0; i < argc; i++) {
        if (strcmp(argv[i], "-n") == 0 && i + 1 < argc)
            target_conns = atoi(argv[++i]);
        else if (strcmp(argv[i], "-w") == 0 && i + 1 < argc)
            hold_sec = atoi(argv[++i]);
    }
    parse_host_port(argc, argv);

    HANDLE iocp = CreateIoCompletionPort(INVALID_HANDLE_VALUE, NULL, 0, 1);
    conn_t* conns = calloc((size_t)target_conns, sizeof(conn_t));
    int connected = 0, ready = 0, completed_conn = 0;

    struct sockaddr_in sa;
    make_addr(&sa);

    sock_t probe = make_socket();
    load_connectex(probe);
    closesocket(probe);

    fprintf(stderr, "memory: connecting %d tls to %s:%d...\n",
            target_conns, g_host, g_port);

    for (int i = 0; i < target_conns; i++) {
        conns[i].sock = make_socket();
        conns[i].payload_size = 64;
        if (conns[i].sock == INVALID_SOCKET) { completed_conn++; continue; }
        if (!post_connect(iocp, &conns[i], &sa)) {
            conn_free(&conns[i]);
            completed_conn++;
        }
    }

    uint64_t hs_deadline = now_us() + 60ULL * 1000000;
    while (ready + (completed_conn - connected) < target_conns &&
           now_us() < hs_deadline) {
        DWORD bytes = 0; ULONG_PTR key = 0; LPOVERLAPPED ov = NULL;
        BOOL ok = GetQueuedCompletionStatus(iocp, &bytes, &key, &ov, 2000);
        if (!ov) break;
        io_op_t* op = (io_op_t*)ov;
        conn_t* c = op->conn;

        if (op->kind == OP_CONNECT) {
            completed_conn++;
            if (ok && conn_start_tls(c)) connected++;
            else conn_free(c);
        } else if (op->kind == OP_RECV) {
            if (!ok || bytes == 0) { conn_free(c); continue; }
            BIO_write(c->rbio, c->net_in, (int)bytes);
            post_recv(c);
            if (c->state == STATE_HANDSHAKING) {
                int r = conn_pump_handshake(c);
                if (r == 0) ready++;
                else if (r < 0) conn_free(c);
            }
        } else if (op->kind == OP_SEND) {
            if (!ok) { conn_free(c); continue; }
            c->out_sent += bytes;
            c->sending = false;
            send_compact(c);
            maybe_send(c);
        }
    }

    fprintf(stderr, "READY %d/%d\n", ready, target_conns);
    Sleep((DWORD)hold_sec * 1000);

    emit_memory_json(target_conns, ready, get_rss_kb());

    for (int i = 0; i < target_conns; i++) {
        if (conns[i].state != STATE_DEAD) conn_free(&conns[i]);
    }
    CloseHandle(iocp);
    free(conns);
}

static BOOL WINAPI win_ctrl_handler(DWORD type) {
    (void)type;
    g_running = false;
    return TRUE;
}

static void platform_init(void) {
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
    SetConsoleCtrlHandler(win_ctrl_handler, TRUE);
    ssl_ctx_init();
}

static void platform_cleanup(void) {
    if (g_ssl_ctx) SSL_CTX_free(g_ssl_ctx);
    WSACleanup();
}

#else /* POSIX */

/* ==========================================================================
 * POSIX backend: epoll (Linux) / kqueue (macOS, *BSD) — readiness model.
 * Sockets are registered for both read+write edge-triggered; the TLS state
 * machine drives on whichever readiness arrives.
 * ========================================================================== */

typedef struct {
    sock_t fd;
    bool   readable;
    bool   writable;
    bool   error;
} poll_ev_t;

#if defined(__linux__)
#include <sys/epoll.h>

static int poller_create(void) { return epoll_create1(0); }

static void poller_add(int pfd, sock_t fd, bool want_rd, bool want_wr) {
    struct epoll_event ev = { .data.fd = fd, .events = EPOLLET };
    if (want_rd) ev.events |= EPOLLIN;
    if (want_wr) ev.events |= EPOLLOUT;
    epoll_ctl(pfd, EPOLL_CTL_ADD, fd, &ev);
}

static void poller_del(int pfd, sock_t fd) {
    epoll_ctl(pfd, EPOLL_CTL_DEL, fd, NULL);
}

static int poller_wait(int pfd, poll_ev_t* out, int max, int timeout_ms) {
    struct epoll_event events[MAX_EVENTS];
    if (max > MAX_EVENTS) max = MAX_EVENTS;
    int nev = epoll_wait(pfd, events, max, timeout_ms);
    for (int i = 0; i < nev; i++) {
        out[i].fd       = events[i].data.fd;
        out[i].readable = (events[i].events & EPOLLIN) != 0;
        out[i].writable = (events[i].events & EPOLLOUT) != 0;
        out[i].error    = (events[i].events & (EPOLLERR | EPOLLHUP)) != 0;
    }
    return nev;
}

#else /* kqueue */
#include <sys/event.h>

static int poller_create(void) { return kqueue(); }

static void poller_add(int pfd, sock_t fd, bool want_rd, bool want_wr) {
    struct kevent ch[2];
    int n = 0;
    if (want_rd)
        EV_SET(&ch[n++], fd, EVFILT_READ, EV_ADD | EV_CLEAR, 0, 0, NULL);
    if (want_wr)
        EV_SET(&ch[n++], fd, EVFILT_WRITE, EV_ADD | EV_CLEAR, 0, 0, NULL);
    if (n) kevent(pfd, ch, n, NULL, 0, NULL);
}

static void poller_del(int pfd, sock_t fd) {
    struct kevent ch[2];
    EV_SET(&ch[0], fd, EVFILT_READ, EV_DELETE, 0, 0, NULL);
    EV_SET(&ch[1], fd, EVFILT_WRITE, EV_DELETE, 0, 0, NULL);
    kevent(pfd, ch, 2, NULL, 0, NULL);
}

static int poller_wait(int pfd, poll_ev_t* out, int max, int timeout_ms) {
    struct kevent events[MAX_EVENTS];
    if (max > MAX_EVENTS) max = MAX_EVENTS;
    struct timespec ts = {
        .tv_sec  = timeout_ms / 1000,
        .tv_nsec = (long)(timeout_ms % 1000) * 1000000L,
    };
    int nev = kevent(pfd, NULL, 0, events, max, &ts);
    int out_n = 0;
    for (int i = 0; i < nev; i++) {
        sock_t fd = (sock_t)events[i].ident;
        int slot = -1;
        for (int j = 0; j < out_n; j++) {
            if (out[j].fd == fd) { slot = j; break; }
        }
        if (slot < 0) {
            slot = out_n++;
            out[slot].fd = fd;
            out[slot].readable = false;
            out[slot].writable = false;
            out[slot].error = false;
        }
        if (events[i].filter == EVFILT_READ)  out[slot].readable = true;
        if (events[i].filter == EVFILT_WRITE) out[slot].writable = true;
        if (events[i].flags & EV_EOF)         out[slot].error = true;
    }
    return out_n;
}
#endif

static void set_nonblocking(sock_t fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

static void sig_handler(int sig) {
    (void)sig;
    g_running = false;
}

/* -------------------------------------------------------------------------- */
/* TLS connection state machine                                               */
/* -------------------------------------------------------------------------- */

typedef enum {
    STATE_CONNECTING,   /* TCP connect() in flight */
    STATE_HANDSHAKING,  /* SSL_connect cycling      */
    STATE_READY,        /* application data         */
    STATE_DEAD,
} conn_state_t;

typedef struct {
    sock_t       fd;
    SSL*         ssl;
    conn_state_t state;
    uint64_t     send_ts;
} conn_t;

/* Open a non-blocking TCP socket + start connect; register with poller. */
static sock_t tls_open(int pfd, struct sockaddr_in* sa, conn_t* slot_base) {
    sock_t fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return BENCH_INVALID_SOCK;
    set_nonblocking(fd);
    int yes = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, (void*)&yes, sizeof(yes));
    int rc = connect(fd, (struct sockaddr*)sa, sizeof(*sa));
    if (rc < 0 && errno != EINPROGRESS) { close(fd); return BENCH_INVALID_SOCK; }
    poller_add(pfd, fd, true, true);

    conn_t* c = &slot_base[fd];
    c->fd    = fd;
    c->ssl   = NULL;
    c->state = STATE_CONNECTING;
    return fd;
}

/* Drive SSL_connect; returns 1 still-handshaking, 0 ready, -1 fatal. */
static int tls_handshake(conn_t* c) {
    int rc = SSL_connect(c->ssl);
    if (rc == 1) { c->state = STATE_READY; return 0; }
    int err = SSL_get_error(c->ssl, rc);
    if (err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE) return 1;
    return -1;
}

/* TCP connect completed -> attach SSL and begin handshake. */
static int tls_begin_handshake(conn_t* c) {
    int soerr = 0; socklen_t len = sizeof(soerr);
    getsockopt(c->fd, SOL_SOCKET, SO_ERROR, &soerr, &len);
    if (soerr != 0) { c->state = STATE_DEAD; return -1; }
    c->ssl = SSL_new(g_ssl_ctx);
    SSL_set_fd(c->ssl, c->fd);
    SSL_set_connect_state(c->ssl);
    c->state = STATE_HANDSHAKING;
    return tls_handshake(c);
}

static void tls_free(conn_t* c) {
    if (c->ssl) {
        SSL_shutdown(c->ssl);
        SSL_free(c->ssl);
        c->ssl = NULL;
    }
    if (c->fd > 0) { close(c->fd); c->fd = BENCH_INVALID_SOCK; }
    c->state = STATE_DEAD;
}

/* ----- THROUGHPUT (POSIX) ----- */

static void run_throughput(int argc, char** argv) {
    int target_conns = 1000;
    int duration_sec = 30;
    int payload_size = 64;

    for (int i = 0; i < argc; i++) {
        if (strcmp(argv[i], "-n") == 0 && i + 1 < argc)
            target_conns = atoi(argv[++i]);
        else if (strcmp(argv[i], "-d") == 0 && i + 1 < argc)
            duration_sec = atoi(argv[++i]);
        else if (strcmp(argv[i], "-s") == 0 && i + 1 < argc)
            payload_size = atoi(argv[++i]);
    }
    parse_host_port(argc, argv);

    if (payload_size < 1) payload_size = 1;
    if (payload_size > 65536) payload_size = 65536;

    int max_fds = target_conns + 100;
    conn_t* conns = calloc((size_t)max_fds, sizeof(conn_t));
    uint64_t* latencies = malloc(LATENCY_SLOTS * sizeof(uint64_t));
    int lat_count = 0;
    int ready = 0, created = 0, dead = 0;
    uint64_t msgs_sent = 0, msgs_recv = 0;

    int epfd = poller_create();

    struct sockaddr_in sa;
    make_addr(&sa);

    fprintf(stderr, "connecting %d tls to %s:%d...\n",
            target_conns, g_host, g_port);

    /* establish: TCP connect + TLS handshake, in batches */
    int issued = 0;
    while (issued < target_conns) {
        int batch = 500;
        if (issued + batch > target_conns) batch = target_conns - issued;
        for (int i = 0; i < batch; i++) {
            if (tls_open(epfd, &sa, conns) != BENCH_INVALID_SOCK) created++;
            issued++;
        }
        poll_ev_t events[MAX_EVENTS];
        int nev = poller_wait(epfd, events, MAX_EVENTS, 10);
        for (int i = 0; i < nev; i++) {
            conn_t* c = &conns[events[i].fd];
            if (c->state == STATE_CONNECTING && (events[i].writable || events[i].readable)) {
                if (tls_begin_handshake(c) == 0) ready++;
            } else if (c->state == STATE_HANDSHAKING) {
                int r = tls_handshake(c);
                if (r == 0) ready++;
                else if (r < 0) { tls_free(c); dead++; }
            }
        }
    }

    /* drain remaining handshakes */
    for (int w = 0; w < 300 && ready + dead < created; w++) {
        poll_ev_t events[MAX_EVENTS];
        int nev = poller_wait(epfd, events, MAX_EVENTS, 100);
        for (int i = 0; i < nev; i++) {
            conn_t* c = &conns[events[i].fd];
            if (c->state == STATE_CONNECTING && (events[i].writable || events[i].readable)) {
                if (tls_begin_handshake(c) == 0) ready++;
            } else if (c->state == STATE_HANDSHAKING) {
                int r = tls_handshake(c);
                if (r == 0) ready++;
                else if (r < 0) { tls_free(c); dead++; }
            }
        }
    }

    fprintf(stderr, "tls connected: %d / %d, starting echo benchmark for %ds...\n",
            ready, target_conns, duration_sec);

    char payload[65536];
    memset(payload, 'A', (size_t)payload_size);

    /* prime: one write per ready connection */
    for (int fd = 0; fd < max_fds; fd++) {
        if (conns[fd].state == STATE_READY) {
            conns[fd].send_ts = now_us();
            if (SSL_write(conns[fd].ssl, payload, payload_size) == payload_size)
                msgs_sent++;
        }
    }

    uint64_t start = now_us();
    uint64_t deadline = start + (uint64_t)duration_sec * 1000000;

    while (g_running && now_us() < deadline) {
        poll_ev_t events[MAX_EVENTS];
        int nev = poller_wait(epfd, events, MAX_EVENTS, 100);
        for (int i = 0; i < nev; i++) {
            if (!events[i].readable) continue;
            conn_t* c = &conns[events[i].fd];
            if (c->state != STATE_READY) continue;
            char buf[65536];
            for (;;) {
                int n = SSL_read(c->ssl, buf, (int)sizeof(buf));
                if (n <= 0) break;
                /* server echoes the full payload back; treat each full
                 * payload as one round-trip. */
                uint64_t lat = now_us() - c->send_ts;
                if (lat_count < LATENCY_SLOTS) latencies[lat_count++] = lat;
                msgs_recv++;
                c->send_ts = now_us();
                if (SSL_write(c->ssl, payload, payload_size) == payload_size)
                    msgs_sent++;
            }
        }
    }

    double elapsed_sec = (double)(now_us() - start) / 1000000.0;
    emit_throughput_json(ready, elapsed_sec, msgs_sent, msgs_recv,
                         latencies, lat_count);

    for (int fd = 0; fd < max_fds; fd++) {
        if (conns[fd].state != STATE_DEAD && conns[fd].fd > 0) tls_free(&conns[fd]);
    }
    close(epfd);
    free(conns);
    free(latencies);
}

/* ----- CONNRATE (POSIX): full TLS handshakes per second ----- */

static void run_connrate(int argc, char** argv) {
    int concurrency  = 256;
    int duration_sec = 10;

    for (int i = 0; i < argc; i++) {
        if (strcmp(argv[i], "-c") == 0 && i + 1 < argc)
            concurrency = atoi(argv[++i]);
        else if (strcmp(argv[i], "-d") == 0 && i + 1 < argc)
            duration_sec = atoi(argv[++i]);
    }
    parse_host_port(argc, argv);

    int max_fds = 65536;
    conn_t* conns = calloc((size_t)max_fds, sizeof(conn_t));
    int epfd = poller_create();
    uint64_t connects_ok = 0, connects_fail = 0;

    struct sockaddr_in sa;
    make_addr(&sa);

    fprintf(stderr, "connrate: %d concurrent tls handshakes to %s:%d for %ds\n",
            concurrency, g_host, g_port, duration_sec);

    for (int i = 0; i < concurrency; i++) tls_open(epfd, &sa, conns);

    uint64_t start = now_us();
    uint64_t deadline = start + (uint64_t)duration_sec * 1000000;
    poll_ev_t events[MAX_EVENTS];

    while (g_running && now_us() < deadline) {
        int nev = poller_wait(epfd, events, MAX_EVENTS, 10);
        for (int i = 0; i < nev; i++) {
            conn_t* c = &conns[events[i].fd];
            int done = 0; /* 1 = ok, -1 = fail */

            if (events[i].error) {
                done = -1;
            } else if (c->state == STATE_CONNECTING) {
                int r = tls_begin_handshake(c);
                if (r == 0) done = 1;
                else if (r < 0) done = -1;
            } else if (c->state == STATE_HANDSHAKING) {
                int r = tls_handshake(c);
                if (r == 0) done = 1;
                else if (r < 0) done = -1;
            }

            if (done != 0) {
                if (done > 0) connects_ok++; else connects_fail++;
                poller_del(epfd, c->fd);
                tls_free(c);
                /* immediately start a fresh handshake to keep pressure up */
                tls_open(epfd, &sa, conns);
            }
        }
    }

    double elapsed_sec = (double)(now_us() - start) / 1000000.0;
    emit_connrate_json(elapsed_sec, concurrency, connects_ok, connects_fail);

    for (int fd = 0; fd < max_fds; fd++) {
        if (conns[fd].state != STATE_DEAD && conns[fd].fd > 0) tls_free(&conns[fd]);
    }
    close(epfd);
    free(conns);
}

/* ----- MEMORY (POSIX): hold N established TLS connections ----- */

static void run_memory(int argc, char** argv) {
    int target_conns = 10000;
    int hold_sec     = 5;

    for (int i = 0; i < argc; i++) {
        if (strcmp(argv[i], "-n") == 0 && i + 1 < argc)
            target_conns = atoi(argv[++i]);
        else if (strcmp(argv[i], "-w") == 0 && i + 1 < argc)
            hold_sec = atoi(argv[++i]);
    }
    parse_host_port(argc, argv);

    int max_fds = target_conns + 100;
    conn_t* conns = calloc((size_t)max_fds, sizeof(conn_t));
    int epfd = poller_create();
    int ready = 0, created = 0, dead = 0;

    struct sockaddr_in sa;
    make_addr(&sa);

    fprintf(stderr, "memory: connecting %d tls to %s:%d...\n",
            target_conns, g_host, g_port);

    int issued = 0;
    while (issued < target_conns) {
        int batch = 500;
        if (issued + batch > target_conns) batch = target_conns - issued;
        for (int i = 0; i < batch; i++) {
            if (tls_open(epfd, &sa, conns) != BENCH_INVALID_SOCK) created++;
            issued++;
        }
        poll_ev_t events[MAX_EVENTS];
        int nev = poller_wait(epfd, events, MAX_EVENTS, 10);
        for (int i = 0; i < nev; i++) {
            conn_t* c = &conns[events[i].fd];
            if (c->state == STATE_CONNECTING && (events[i].writable || events[i].readable)) {
                if (tls_begin_handshake(c) == 0) ready++;
            } else if (c->state == STATE_HANDSHAKING) {
                int r = tls_handshake(c);
                if (r == 0) ready++;
                else if (r < 0) { tls_free(c); dead++; }
            }
        }
    }

    for (int w = 0; w < 300 && ready + dead < created; w++) {
        poll_ev_t events[MAX_EVENTS];
        int nev = poller_wait(epfd, events, MAX_EVENTS, 100);
        for (int i = 0; i < nev; i++) {
            conn_t* c = &conns[events[i].fd];
            if (c->state == STATE_CONNECTING && (events[i].writable || events[i].readable)) {
                if (tls_begin_handshake(c) == 0) ready++;
            } else if (c->state == STATE_HANDSHAKING) {
                int r = tls_handshake(c);
                if (r == 0) ready++;
                else if (r < 0) { tls_free(c); dead++; }
            }
        }
    }

    fprintf(stderr, "READY %d/%d\n", ready, target_conns);
    sleep((unsigned)hold_sec);

    emit_memory_json(target_conns, ready, get_rss_kb());

    for (int fd = 0; fd < max_fds; fd++) {
        if (conns[fd].state != STATE_DEAD && conns[fd].fd > 0) tls_free(&conns[fd]);
    }
    close(epfd);
    free(conns);
}

static void platform_init(void) {
    signal(SIGINT, sig_handler);
    signal(SIGPIPE, SIG_IGN);
    struct rlimit rl = { .rlim_cur = 200000, .rlim_max = 200000 };
    setrlimit(RLIMIT_NOFILE, &rl);
    ssl_ctx_init();
}

static void platform_cleanup(void) {
    if (g_ssl_ctx) SSL_CTX_free(g_ssl_ctx);
}

#endif /* _WIN32 / POSIX */

/* ========================================================================== */
/* MAIN                                                                       */
/* ========================================================================== */

int main(int argc, char** argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: tls-bench <mode> [options]\n");
        fprintf(stderr, "modes:\n");
        fprintf(stderr, "  throughput  -n conns -d sec -s payload -h host -p port\n");
        fprintf(stderr, "  connrate    -c concurrency -d sec -h host -p port\n");
        fprintf(stderr, "  memory      -n conns -w hold_sec -h host -p port\n");
        return 1;
    }

    platform_init();

    const char* mode = argv[1];
    int sub_argc = argc - 2;
    char** sub_argv = argv + 2;

    if (strcmp(mode, "throughput") == 0) {
        run_throughput(sub_argc, sub_argv);
    } else if (strcmp(mode, "connrate") == 0) {
        run_connrate(sub_argc, sub_argv);
    } else if (strcmp(mode, "memory") == 0) {
        run_memory(sub_argc, sub_argv);
    } else {
        fprintf(stderr, "unknown mode: %s\n", mode);
        platform_cleanup();
        return 1;
    }

    platform_cleanup();
    return 0;
}
