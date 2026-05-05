#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#pragma comment(lib, "ws2_32.lib")

static const char* g_host = "127.0.0.1";
static int         g_port = 9000;
static volatile LONG g_running = 1;

static int g_payload_size = 64;

static volatile LONG64 g_msgs_sent = 0;
static volatile LONG64 g_msgs_recv = 0;

#define MAX_LATENCY_SAMPLES 4000000
static uint64_t g_latencies[MAX_LATENCY_SAMPLES];
static volatile LONG g_lat_count = 0;

static uint64_t now_us(void) {
    static LARGE_INTEGER freq = {0};
    if (freq.QuadPart == 0) QueryPerformanceFrequency(&freq);
    LARGE_INTEGER cnt;
    QueryPerformanceCounter(&cnt);
    return (uint64_t)(cnt.QuadPart * 1000000 / freq.QuadPart);
}

static BOOL WINAPI ctrl_handler(DWORD type) {
    (void)type;
    InterlockedExchange(&g_running, 0);
    return TRUE;
}

static int cmp_u64(const void* a, const void* b) {
    uint64_t va = *(const uint64_t*)a;
    uint64_t vb = *(const uint64_t*)b;
    if (va < vb) return -1;
    if (va > vb) return 1;
    return 0;
}

static DWORD WINAPI worker_thread(LPVOID param) {
    SOCKET s = (SOCKET)(uintptr_t)param;
    char* sendbuf = (char*)malloc(g_payload_size);
    char* recvbuf = (char*)malloc(65536);
    memset(sendbuf, 'A', g_payload_size);

    while (InterlockedOr(&g_running, 0)) {
        uint64_t t0 = now_us();

        /* send full payload */
        int total_sent = 0;
        while (total_sent < g_payload_size) {
            int n = send(s, sendbuf + total_sent, g_payload_size - total_sent, 0);
            if (n <= 0) goto done;
            total_sent += n;
        }
        InterlockedIncrement64(&g_msgs_sent);

        /* recv full echo */
        int total_recv = 0;
        while (total_recv < g_payload_size) {
            int n = recv(s, recvbuf + total_recv, g_payload_size - total_recv, 0);
            if (n <= 0) goto done;
            total_recv += n;
        }
        InterlockedIncrement64(&g_msgs_recv);

        uint64_t lat = now_us() - t0;
        LONG idx = InterlockedIncrement(&g_lat_count) - 1;
        if (idx < MAX_LATENCY_SAMPLES) g_latencies[idx] = lat;
    }

done:
    free(sendbuf);
    free(recvbuf);
    closesocket(s);
    return 0;
}

static void run_throughput(int argc, char** argv) {
    int target_conns = 1000;
    int duration_sec = 10;

    for (int i = 0; i < argc; i++) {
        if (strcmp(argv[i], "-n") == 0 && i + 1 < argc) target_conns = atoi(argv[++i]);
        else if (strcmp(argv[i], "-d") == 0 && i + 1 < argc) duration_sec = atoi(argv[++i]);
        else if (strcmp(argv[i], "-s") == 0 && i + 1 < argc) g_payload_size = atoi(argv[++i]);
        else if (strcmp(argv[i], "-h") == 0 && i + 1 < argc) g_host = argv[++i];
        else if (strcmp(argv[i], "-p") == 0 && i + 1 < argc) g_port = atoi(argv[++i]);
    }

    if (g_payload_size < 1) g_payload_size = 1;
    if (g_payload_size > 65536) g_payload_size = 65536;

    fprintf(stderr, "throughput: conns=%d dur=%ds payload=%dB host=%s port=%d\n",
            target_conns, duration_sec, g_payload_size, g_host, g_port);

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((u_short)g_port);
    inet_pton(AF_INET, g_host, &addr.sin_addr);

    HANDLE* threads = (HANDLE*)calloc(target_conns, sizeof(HANDLE));
    int connected = 0;

    for (int i = 0; i < target_conns; i++) {
        SOCKET s = socket(AF_INET, SOCK_STREAM, 0);
        if (s == INVALID_SOCKET) {
            fprintf(stderr, "socket() failed at conn %d: %d\n", i, WSAGetLastError());
            break;
        }
        int one = 1;
        setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (char*)&one, sizeof(one));

        if (connect(s, (struct sockaddr*)&addr, sizeof(addr)) != 0) {
            fprintf(stderr, "connect() failed at conn %d: %d\n", i, WSAGetLastError());
            closesocket(s);
            break;
        }

        threads[i] = CreateThread(NULL, 0, worker_thread, (LPVOID)(uintptr_t)s, 0, NULL);
        if (!threads[i]) {
            closesocket(s);
            break;
        }
        connected++;
    }

    fprintf(stderr, "connected %d/%d, running for %ds...\n", connected, target_conns, duration_sec);
    if (connected == 0) goto done;

    /* warmup 2s */
    Sleep(2000);
    InterlockedExchange64(&g_msgs_sent, 0);
    InterlockedExchange64(&g_msgs_recv, 0);
    InterlockedExchange(&g_lat_count, 0);

    uint64_t start_us = now_us();
    Sleep(duration_sec * 1000);
    InterlockedExchange(&g_running, 0);

    uint64_t elapsed_us = now_us() - start_us;
    double elapsed_sec = (double)elapsed_us / 1000000.0;

    /* wait for threads */
    WaitForMultipleObjects(connected < MAXIMUM_WAIT_OBJECTS ? connected : MAXIMUM_WAIT_OBJECTS,
                           threads, TRUE, 5000);
    /* handle >64 threads */
    for (int i = MAXIMUM_WAIT_OBJECTS; i < connected; i += MAXIMUM_WAIT_OBJECTS) {
        int batch = connected - i;
        if (batch > MAXIMUM_WAIT_OBJECTS) batch = MAXIMUM_WAIT_OBJECTS;
        WaitForMultipleObjects(batch, threads + i, TRUE, 5000);
    }

    LONG64 msgs_recv = InterlockedOr64(&g_msgs_recv, 0);
    LONG64 msgs_sent = InterlockedOr64(&g_msgs_sent, 0);
    LONG lat_count = InterlockedOr(&g_lat_count, 0);

    uint64_t p50 = 0, p99 = 0, pmax = 0;
    if (lat_count > 0) {
        if (lat_count > MAX_LATENCY_SAMPLES) lat_count = MAX_LATENCY_SAMPLES;
        qsort(g_latencies, lat_count, sizeof(uint64_t), cmp_u64);
        p50 = g_latencies[(int)(lat_count * 0.5)];
        p99 = g_latencies[(int)(lat_count * 0.99)];
        pmax = g_latencies[lat_count - 1];
    }

    uint64_t tp = (uint64_t)((double)msgs_recv / elapsed_sec);

    printf("{\n");
    printf("  \"connections\": %d,\n", connected);
    printf("  \"duration_sec\": %.2f,\n", elapsed_sec);
    printf("  \"messages_sent\": %lld,\n", (long long)msgs_sent);
    printf("  \"messages_recv\": %lld,\n", (long long)msgs_recv);
    printf("  \"throughput_msg_per_sec\": %lld,\n", (long long)tp);
    printf("  \"latency_p50_us\": %lld,\n", (long long)p50);
    printf("  \"latency_p99_us\": %lld,\n", (long long)p99);
    printf("  \"latency_max_us\": %lld\n", (long long)pmax);
    printf("}\n");

done:
    for (int i = 0; i < connected; i++) {
        if (threads[i]) CloseHandle(threads[i]);
    }
    free(threads);
}

static volatile LONG64 g_total_connects = 0;
static volatile LONG64 g_failed_connects = 0;

static DWORD WINAPI connrate_worker(LPVOID param) {
    int concurrency = (int)(intptr_t)param;
    (void)concurrency;

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((u_short)g_port);
    inet_pton(AF_INET, g_host, &addr.sin_addr);

    struct linger lg = {1, 0}; /* force RST on close, skip TIME_WAIT */

    while (InterlockedOr(&g_running, 0)) {
        SOCKET s = socket(AF_INET, SOCK_STREAM, 0);
        if (s == INVALID_SOCKET) {
            InterlockedIncrement64(&g_failed_connects);
            Sleep(1);
            continue;
        }

        setsockopt(s, SOL_SOCKET, SO_LINGER, (char*)&lg, sizeof(lg));

        if (connect(s, (struct sockaddr*)&addr, sizeof(addr)) == 0) {
            InterlockedIncrement64(&g_total_connects);
        } else {
            InterlockedIncrement64(&g_failed_connects);
        }
        closesocket(s);
    }
    return 0;
}

static void run_connrate(int argc, char** argv) {
    int concurrency = 64;
    int duration_sec = 10;

    for (int i = 0; i < argc; i++) {
        if (strcmp(argv[i], "-c") == 0 && i + 1 < argc) concurrency = atoi(argv[++i]);
        else if (strcmp(argv[i], "-d") == 0 && i + 1 < argc) duration_sec = atoi(argv[++i]);
        else if (strcmp(argv[i], "-h") == 0 && i + 1 < argc) g_host = argv[++i];
        else if (strcmp(argv[i], "-p") == 0 && i + 1 < argc) g_port = atoi(argv[++i]);
    }

    fprintf(stderr, "connrate: concurrency=%d dur=%ds host=%s port=%d\n",
            concurrency, duration_sec, g_host, g_port);

    HANDLE* threads = (HANDLE*)calloc(concurrency, sizeof(HANDLE));
    for (int i = 0; i < concurrency; i++) {
        threads[i] = CreateThread(NULL, 0, connrate_worker, (LPVOID)(intptr_t)concurrency, 0, NULL);
    }

    /* warmup 1s */
    Sleep(1000);
    InterlockedExchange64(&g_total_connects, 0);
    InterlockedExchange64(&g_failed_connects, 0);

    uint64_t start_us = now_us();
    Sleep(duration_sec * 1000);
    InterlockedExchange(&g_running, 0);
    uint64_t elapsed_us = now_us() - start_us;

    for (int i = 0; i < concurrency; i += MAXIMUM_WAIT_OBJECTS) {
        int batch = concurrency - i;
        if (batch > MAXIMUM_WAIT_OBJECTS) batch = MAXIMUM_WAIT_OBJECTS;
        WaitForMultipleObjects(batch, threads + i, TRUE, 5000);
    }

    double elapsed_sec_f = (double)elapsed_us / 1000000.0;
    LONG64 total = InterlockedOr64(&g_total_connects, 0);
    LONG64 failed = InterlockedOr64(&g_failed_connects, 0);
    LONG64 cps = (LONG64)((double)total / elapsed_sec_f);

    printf("{\n");
    printf("  \"duration_sec\": %.2f,\n", elapsed_sec_f);
    printf("  \"concurrency\": %d,\n", concurrency);
    printf("  \"total_connects\": %lld,\n", (long long)total);
    printf("  \"failed_connects\": %lld,\n", (long long)failed);
    printf("  \"connects_per_sec\": %lld\n", (long long)cps);
    printf("}\n");

    for (int i = 0; i < concurrency; i++) {
        if (threads[i]) CloseHandle(threads[i]);
    }
    free(threads);
}

int main(int argc, char** argv) {
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
    SetConsoleCtrlHandler(ctrl_handler, TRUE);

    if (argc < 2) {
        fprintf(stderr, "usage: tcp-bench-win <mode> [options]\n");
        fprintf(stderr, "  throughput -n conns -d sec -s payload -h host -p port\n");
        fprintf(stderr, "  connrate   -c concurrency -d sec -h host -p port\n");
        return 1;
    }

    if (strcmp(argv[1], "throughput") == 0) {
        run_throughput(argc - 2, argv + 2);
    } else if (strcmp(argv[1], "connrate") == 0) {
        run_connrate(argc - 2, argv + 2);
    } else {
        fprintf(stderr, "unknown mode: %s\n", argv[1]);
        return 1;
    }

    WSACleanup();
    return 0;
}
