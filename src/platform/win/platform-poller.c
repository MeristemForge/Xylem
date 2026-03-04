/** Copyright (c) 2026-2036, Jin.Wu <wujin.developer@gmail.com>
 *
 *  Permission is hereby granted, free of charge, to any person obtaining a copy
 *  of this software and associated documentation files (the "Software"), to
 *  deal in the Software without restriction, including without limitation the
 *  rights to use, copy, modify, merge, publish, distribute, sublicense, and/or
 *  sell copies of the Software, and to permit persons to whom the Software is
 *  furnished to do so, subject to the following conditions:
 *
 *  The above copyright notice and this permission notice shall be included in
 *  all copies or substantial portions of the Software.
 *
 *  THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 *  IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 *  FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 *  AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 *  LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 *  FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 *  IN THE SOFTWARE.
 */

#include "platform/platform-poller.h"
#include <stdlib.h>
#include <string.h>

/*
 * Internal per-fd tracking structure. Each registered fd gets one of these
 * allocated. It holds two OVERLAPPED structs (one for read probe, one for
 * write probe) and the user data pointer.
 */

typedef struct _poller_probe_s {
    OVERLAPPED           ov;
    platform_poller_op_t dir; /* PLATFORM_POLLER_RD_OP or WR_OP */
} _poller_probe_t;

typedef struct _poller_entry_s {
    SOCKET               sock;
    void*                ud;
    platform_poller_op_t interest; /* current interest set */
    _poller_probe_t      rd_probe;
    _poller_probe_t      wr_probe;
} _poller_entry_t;

/*
 * Simple fd->entry map. Uses a flat array with linear scan.
 * Sufficient for moderate fd counts typical in this use case.
 */
#define _POLLER_MAP_INIT_CAP 16

typedef struct _poller_map_s {
    _poller_entry_t** entries;
    int32_t           count;
    int32_t           cap;
} _poller_map_t;

static _poller_map_t _map = {NULL, 0, 0};

static _poller_entry_t* _poller_map_find(SOCKET sock) {
    for (int32_t i = 0; i < _map.count; i++) {
        if (_map.entries[i] && _map.entries[i]->sock == sock) {
            return _map.entries[i];
        }
    }
    return NULL;
}

static void _poller_map_insert(_poller_entry_t* entry) {
    if (_map.count == _map.cap) {
        int32_t new_cap = _map.cap == 0 ? _POLLER_MAP_INIT_CAP : _map.cap * 2;
        _poller_entry_t** tmp =
            realloc(_map.entries, sizeof(_poller_entry_t*) * new_cap);
        if (!tmp) {
            return;
        }
        _map.entries = tmp;
        _map.cap     = new_cap;
    }
    _map.entries[_map.count++] = entry;
}

static void _poller_map_remove(SOCKET sock) {
    for (int32_t i = 0; i < _map.count; i++) {
        if (_map.entries[i] && _map.entries[i]->sock == sock) {
            free(_map.entries[i]);
            _map.entries[i] = _map.entries[_map.count - 1];
            _map.entries[_map.count - 1] = NULL;
            _map.count--;
            return;
        }
    }
}

static void _poller_map_destroy(void) {
    for (int32_t i = 0; i < _map.count; i++) {
        free(_map.entries[i]);
    }
    free(_map.entries);
    _map.entries = NULL;
    _map.count   = 0;
    _map.cap     = 0;
}

/* Post a zero-byte WSARecv to probe for read readiness. */
static void _poller_post_rd_probe(_poller_entry_t* entry) {
    WSABUF buf   = {0, NULL};
    DWORD  recv  = 0;
    DWORD  flags = 0;

    memset(&entry->rd_probe.ov, 0, sizeof(OVERLAPPED));
    entry->rd_probe.dir = PLATFORM_POLLER_RD_OP;

    WSARecv(entry->sock, &buf, 1, &recv, &flags, &entry->rd_probe.ov, NULL);
}

/* Post a zero-byte WSASend to probe for write readiness. */
static void _poller_post_wr_probe(_poller_entry_t* entry) {
    WSABUF buf  = {0, NULL};
    DWORD  sent = 0;

    memset(&entry->wr_probe.ov, 0, sizeof(OVERLAPPED));
    entry->wr_probe.dir = PLATFORM_POLLER_WR_OP;

    WSASend(entry->sock, &buf, 1, &sent, 0, &entry->wr_probe.ov, NULL);
}

void platform_poller_init(platform_poller_sq_t* sq) {
    *sq = CreateIoCompletionPort(INVALID_HANDLE_VALUE, NULL, 0, 0);
}

void platform_poller_destroy(platform_poller_sq_t* sq) {
    _poller_map_destroy();
    CloseHandle(*sq);
}

void platform_poller_add(platform_poller_sq_t* sq, platform_poller_sqe_t* sqe) {
    _poller_entry_t* entry = calloc(1, sizeof(_poller_entry_t));
    if (!entry) {
        return;
    }
    entry->sock     = sqe->fd;
    entry->ud       = sqe->ud;
    entry->interest = sqe->op;

    HANDLE h = CreateIoCompletionPort(
        (HANDLE)sqe->fd, *sq, (ULONG_PTR)entry, 0);
    if (!h) {
        free(entry);
        return;
    }

    _poller_map_insert(entry);

    if (sqe->op & PLATFORM_POLLER_RD_OP) {
        _poller_post_rd_probe(entry);
    }
    if (sqe->op & PLATFORM_POLLER_WR_OP) {
        _poller_post_wr_probe(entry);
    }
}

void platform_poller_mod(platform_poller_sq_t* sq, platform_poller_sqe_t* sqe) {
    (void)sq;

    _poller_entry_t* entry = _poller_map_find(sqe->fd);
    if (!entry) {
        return;
    }

    platform_poller_op_t old_interest = entry->interest;
    entry->interest = sqe->op;
    entry->ud       = sqe->ud;

    /* cancel pending probes for directions we no longer want */
    if ((old_interest & PLATFORM_POLLER_RD_OP) &&
        !(sqe->op & PLATFORM_POLLER_RD_OP)) {
        CancelIoEx((HANDLE)entry->sock, &entry->rd_probe.ov);
    }
    if ((old_interest & PLATFORM_POLLER_WR_OP) &&
        !(sqe->op & PLATFORM_POLLER_WR_OP)) {
        CancelIoEx((HANDLE)entry->sock, &entry->wr_probe.ov);
    }

    /* post probes for newly added directions */
    if (!(old_interest & PLATFORM_POLLER_RD_OP) &&
        (sqe->op & PLATFORM_POLLER_RD_OP)) {
        _poller_post_rd_probe(entry);
    }
    if (!(old_interest & PLATFORM_POLLER_WR_OP) &&
        (sqe->op & PLATFORM_POLLER_WR_OP)) {
        _poller_post_wr_probe(entry);
    }
}

void platform_poller_del(platform_poller_sq_t* sq, platform_poller_sqe_t* sqe) {
    (void)sq;
    CancelIoEx((HANDLE)sqe->fd, NULL);
    _poller_map_remove(sqe->fd);
}

int platform_poller_wait(
    platform_poller_sq_t* sq, platform_poller_cqe_t* cqe, int timeout) {
    OVERLAPPED_ENTRY entries[PLATFORM_POLLER_CQE_NUM];
    ULONG            count = 0;

    memset(cqe, 0, sizeof(platform_poller_cqe_t) * PLATFORM_POLLER_CQE_NUM);

    BOOL ok = GetQueuedCompletionStatusEx(
        *sq, entries, PLATFORM_POLLER_CQE_NUM, &count, (DWORD)timeout, FALSE);
    if (!ok) {
        return 0;
    }

    /*
     * Merge completions for the same fd into a single cqe.
     * Use completion key (entry pointer) as the merge key.
     */
    ULONG_PTR keys[PLATFORM_POLLER_CQE_NUM];
    int       out = 0;

    for (ULONG i = 0; i < count; i++) {
        _poller_entry_t* entry = (_poller_entry_t*)entries[i].lpCompletionKey;
        _poller_probe_t* probe = (_poller_probe_t*)entries[i].lpOverlapped;

        if (!entry || !probe) {
            continue;
        }

        /* skip cancelled probes (from mod/del) */
        if (!(entry->interest & probe->dir)) {
            continue;
        }

        int found = -1;
        for (int j = 0; j < out; j++) {
            if (keys[j] == (ULONG_PTR)entry) {
                found = j;
                break;
            }
        }

        if (found >= 0) {
            cqe[found].op |= probe->dir;
        } else {
            keys[out]   = (ULONG_PTR)entry;
            cqe[out].ud = entry->ud;
            cqe[out].op = probe->dir;
            out++;
        }

        /* re-arm the probe for next wait cycle */
        if (probe->dir == PLATFORM_POLLER_RD_OP) {
            _poller_post_rd_probe(entry);
        } else if (probe->dir == PLATFORM_POLLER_WR_OP) {
            _poller_post_wr_probe(entry);
        }
    }
    return out;
}
