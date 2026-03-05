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

/* ---------- undocumented NT / AFD types ---------- */

typedef LONG NTSTATUS;

#ifndef STATUS_SUCCESS
#define STATUS_SUCCESS ((NTSTATUS)0x00000000L)
#endif
#ifndef STATUS_PENDING
#define STATUS_PENDING ((NTSTATUS)0x00000103L)
#endif
#ifndef STATUS_CANCELLED
#define STATUS_CANCELLED ((NTSTATUS)0xC0000120L)
#endif

typedef struct _IO_STATUS_BLOCK {
    union {
        NTSTATUS Status;
        PVOID    Pointer;
    };
    ULONG_PTR Information;
} IO_STATUS_BLOCK, *PIO_STATUS_BLOCK;

#define AFD_POLL_RECEIVE           0x0001
#define AFD_POLL_RECEIVE_EXPEDITED 0x0002
#define AFD_POLL_SEND              0x0004

#define AFD_POLL_DISCONNECT        0x0008
#define AFD_POLL_ABORT             0x0010
#define AFD_POLL_LOCAL_CLOSE       0x0020
#define AFD_POLL_ACCEPT            0x0080
#define AFD_POLL_CONNECT_FAIL      0x0100

/*
 * IOCTL code for AFD_POLL.
 * METHOD_BUFFERED = 0, FILE_DEVICE_NETWORK = 0x12
 * Function code 9 => (0x12 << 16) | (9 << 2) = 0x00012024
 */
#define IOCTL_AFD_POLL 0x00012024

typedef struct _AFD_POLL_HANDLE_INFO {
    HANDLE   Handle;
    ULONG    Events;
    NTSTATUS Status;
} AFD_POLL_HANDLE_INFO, *PAFD_POLL_HANDLE_INFO;

typedef struct _AFD_POLL_INFO {
    LARGE_INTEGER        Timeout;
    ULONG                NumberOfHandles;
    ULONG                Exclusive;
    AFD_POLL_HANDLE_INFO Handles[1];
} AFD_POLL_INFO, *PAFD_POLL_INFO;

typedef NTSTATUS(NTAPI* _NtDeviceIoControlFile_fn)(
    HANDLE           FileHandle,
    HANDLE           Event,
    PVOID            ApcRoutine,
    PVOID            ApcContext,
    PIO_STATUS_BLOCK IoStatusBlock,
    ULONG            IoControlCode,
    PVOID            InputBuffer,
    ULONG            InputBufferLength,
    PVOID            OutputBuffer,
    ULONG            OutputBufferLength);

static _NtDeviceIoControlFile_fn _NtDeviceIoControlFile;

/* ---------- internal state embedded in sqe->_reserved ---------- */

typedef struct _poller_state_s {
    IO_STATUS_BLOCK      iosb;
    AFD_POLL_INFO        poll_info;
    platform_poller_op_t interest;
    SOCKET               peer_sock;
} _poller_state_t;

static _poller_state_t* _poller_get_state(platform_poller_sqe_t* sqe) {
    return (_poller_state_t*)sqe->_reserved;
}

/* Create a peer socket using the same MSAFD provider as the target. */
static SOCKET _poller_create_peer(SOCKET target) {
    WSAPROTOCOL_INFOW info;
    int               len = sizeof(info);

    if (getsockopt(target, SOL_SOCKET, SO_PROTOCOL_INFOW,
                   (char*)&info, &len) == SOCKET_ERROR) {
        return INVALID_SOCKET;
    }

    SOCKET peer = WSASocketW(
        info.iAddressFamily, info.iSocketType, info.iProtocol,
        &info, 0, WSA_FLAG_OVERLAPPED);
    return peer;
}

static ULONG _poller_op_to_afd_events(platform_poller_op_t op) {
    ULONG events = 0;
    if (op & PLATFORM_POLLER_RD_OP) {
        events |= AFD_POLL_RECEIVE | AFD_POLL_DISCONNECT |
                  AFD_POLL_ACCEPT | AFD_POLL_ABORT;
    }
    if (op & PLATFORM_POLLER_WR_OP) {
        events |= AFD_POLL_SEND | AFD_POLL_CONNECT_FAIL;
    }
    return events;
}

static platform_poller_op_t _poller_afd_events_to_op(ULONG events) {
    platform_poller_op_t op = PLATFORM_POLLER_NO_OP;
    if (events & (AFD_POLL_RECEIVE | AFD_POLL_DISCONNECT |
                  AFD_POLL_ACCEPT | AFD_POLL_ABORT)) {
        op |= PLATFORM_POLLER_RD_OP;
    }
    if (events & (AFD_POLL_SEND | AFD_POLL_CONNECT_FAIL)) {
        op |= PLATFORM_POLLER_WR_OP;
    }
    return op;
}

/* Submit an AFD_POLL IOCTL for the given sqe. */
static void _poller_submit_poll(platform_poller_sqe_t* sqe) {
    _poller_state_t* st = _poller_get_state(sqe);

    memset(&st->iosb, 0, sizeof(IO_STATUS_BLOCK));
    st->iosb.Status = STATUS_PENDING;

    memset(&st->poll_info, 0, sizeof(AFD_POLL_INFO));
    st->poll_info.Timeout.QuadPart  = INT64_MAX;
    st->poll_info.NumberOfHandles   = 1;
    st->poll_info.Exclusive         = FALSE;
    st->poll_info.Handles[0].Handle = (HANDLE)sqe->fd;
    st->poll_info.Handles[0].Events = _poller_op_to_afd_events(st->interest);
    st->poll_info.Handles[0].Status = 0;

    _NtDeviceIoControlFile(
        (HANDLE)st->peer_sock,
        NULL,
        NULL,
        (PVOID)sqe,
        &st->iosb,
        IOCTL_AFD_POLL,
        &st->poll_info,
        sizeof(AFD_POLL_INFO),
        &st->poll_info,
        sizeof(AFD_POLL_INFO));
}

static void _poller_cancel_poll(platform_poller_sqe_t* sqe) {
    _poller_state_t* st = _poller_get_state(sqe);

    if (st->iosb.Status == STATUS_PENDING) {
        CancelIoEx((HANDLE)st->peer_sock, (LPOVERLAPPED)&st->iosb);
    }
}

/* ---------- public API ---------- */

int platform_poller_init(platform_poller_sq_t* sq) {
    if (!_NtDeviceIoControlFile) {
        HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
        _NtDeviceIoControlFile = (_NtDeviceIoControlFile_fn)
            GetProcAddress(ntdll, "NtDeviceIoControlFile");
    }
    *sq = CreateIoCompletionPort(INVALID_HANDLE_VALUE, NULL, 0, 0);
    return (*sq == NULL) ? -1 : 0;
}

void platform_poller_destroy(platform_poller_sq_t* sq) {
    CloseHandle(*sq);
}

int platform_poller_add(platform_poller_sq_t* sq, platform_poller_sqe_t* sqe) {
    _poller_state_t* st = _poller_get_state(sqe);
    memset(st, 0, sizeof(_poller_state_t));
    st->interest = sqe->op;

    st->peer_sock = _poller_create_peer(sqe->fd);
    if (st->peer_sock == INVALID_SOCKET) {
        return -1;
    }

    HANDLE h = CreateIoCompletionPort(
        (HANDLE)st->peer_sock, *sq, 0, 0);
    if (!h) {
        closesocket(st->peer_sock);
        st->peer_sock = INVALID_SOCKET;
        return -1;
    }

    SetFileCompletionNotificationModes(
        (HANDLE)st->peer_sock,
        FILE_SKIP_SET_EVENT_ON_HANDLE);

    _poller_submit_poll(sqe);
    return 0;
}

int platform_poller_mod(platform_poller_sq_t* sq, platform_poller_sqe_t* sqe) {
    (void)sq;
    _poller_state_t* st = _poller_get_state(sqe);

    _poller_cancel_poll(sqe);
    st->interest = sqe->op;
    _poller_submit_poll(sqe);
    return 0;
}

int platform_poller_del(platform_poller_sq_t* sq, platform_poller_sqe_t* sqe) {
    (void)sq;
    _poller_state_t* st = _poller_get_state(sqe);

    _poller_cancel_poll(sqe);

    if (st->peer_sock != INVALID_SOCKET) {
        closesocket(st->peer_sock);
        st->peer_sock = INVALID_SOCKET;
    }
    return 0;
}

int platform_poller_wait(
    platform_poller_sq_t* sq, platform_poller_cqe_t* cqe,
    int max_events, int timeout) {
    OVERLAPPED_ENTRY* entries =
        (OVERLAPPED_ENTRY*)malloc(sizeof(OVERLAPPED_ENTRY) * max_events);
    ULONG count = 0;

    memset(cqe, 0, sizeof(platform_poller_cqe_t) * max_events);

    BOOL ok = GetQueuedCompletionStatusEx(
        *sq, entries, (ULONG)max_events, &count, (DWORD)timeout, FALSE);
    if (!ok) {
        DWORD err = GetLastError();
        free(entries);
        if (err == WAIT_TIMEOUT) {
            return 0;
        }
        return -1;
    }

    ULONG_PTR* keys =
        (ULONG_PTR*)malloc(sizeof(ULONG_PTR) * max_events);
    if (!keys) {
        free(entries);
        return -1;
    }
    int out = 0;

    for (ULONG i = 0; i < count; i++) {
        platform_poller_sqe_t* completed_sqe =
            (platform_poller_sqe_t*)entries[i].lpOverlapped;

        if (!completed_sqe) {
            continue;
        }

        _poller_state_t* st = _poller_get_state(completed_sqe);

        if (st->iosb.Status == STATUS_CANCELLED) {
            continue;
        }

        ULONG              result_events = st->poll_info.Handles[0].Events;
        platform_poller_op_t fired = _poller_afd_events_to_op(result_events);

        fired &= st->interest;
        if (fired == PLATFORM_POLLER_NO_OP) {
            /* spurious wakeup, re-arm */
            _poller_submit_poll(completed_sqe);
            continue;
        }

        int found = -1;
        for (int j = 0; j < out; j++) {
            if (keys[j] == (ULONG_PTR)completed_sqe) {
                found = j;
                break;
            }
        }

        if (found >= 0) {
            cqe[found].op |= fired;
        } else {
            keys[out]   = (ULONG_PTR)completed_sqe;
            cqe[out].ud = completed_sqe->ud;
            cqe[out].op = fired;
            out++;
        }
    }

    free(keys);
    free(entries);
    return out;
}
