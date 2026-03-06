/* Windows IOCP-based ae.c module
 *
 * Copyright (c) 2024
 * All rights reserved.
 */

#include <winsock2.h>
#include <mswsock.h>
#include <windows.h>
#include "ae.h"

#define MAX_BUFFER_SIZE 8192
#define MAX_LISTEN_SOCKETS 64

/* We distinguish read vs write completions by comparing the overlapped pointer
 * to &buf->read_overlapped or &buf->write_overlapped. No need to tag hEvent. */
#define IOCP_OP_READ  ((HANDLE)(DWORD_PTR)0)
#define IOCP_OP_WRITE ((HANDLE)(DWORD_PTR)0)

static int wsaInitialized = 0;

static int initializeWSA(void) {
    if (!wsaInitialized) {
        WSADATA wsaData;
        if (WSAStartup(MAKEWORD(2,2), &wsaData) != 0) {
            return -1;
        }
        wsaInitialized = 1;
    }
    return 0;
}

static void cleanupWSA(void) {
    if (wsaInitialized) {
        WSACleanup();
        wsaInitialized = 0;
    }
}

/* Per-fd buffer: separate overlapped for read and write so we can wait on both. */
typedef struct aeIocpBuffer {
    SOCKET fd;
    int closing;    /* 1 = all events removed, awaiting drain of IOCP completions */
    /* read */
    WSAOVERLAPPED read_overlapped;
    WSABUF read_wsaBuf;
    char read_buffer[MAX_BUFFER_SIZE];
    /* pending read: data already received by WSARecv, not yet consumed by application read() */
    char *pending_buf;
    size_t pending_len;
    size_t pending_off;
    /* write: synthetic completion via PostQueuedCompletionStatus.
     * CancelIoEx cannot cancel these, so we use a generation counter to
     * detect stale completions.  The 'bytes' field of the posted completion
     * carries the generation at post-time; poll compares it. */
    WSAOVERLAPPED write_overlapped;
    DWORD write_gen;
    WSABUF write_wsaBuf;
    char write_buffer[1];
} aeIocpBuffer;

/* Listen socket state: WSAEventSelect + thread posts to IOCP when FD_ACCEPT */
typedef struct aeIocpListen {
    CRITICAL_SECTION lock;
    int count;
    int fds[MAX_LISTEN_SOCKETS];
    WSAEVENT events[MAX_LISTEN_SOCKETS];
    HANDLE update_event;   /* manual-reset, signaled when list changed */
    HANDLE thread;
    volatile long stop;
} aeIocpListen;

typedef struct aeApiState {
    HANDLE iocpHandle;
    aeIocpBuffer **buffers;
    aeIocpListen *listen;
} aeApiState;

static int is_listen_socket(SOCKET sock) {
    SOCKADDR addr;
    int addrlen = sizeof(addr);
    if (getsockname(sock, &addr, &addrlen) != 0) return 0;
    if (addr.sa_family != AF_INET && addr.sa_family != AF_INET6) return 0;
    int optval = 0;
    int optlen = sizeof(optval);
    if (getsockopt(sock, SOL_SOCKET, SO_ACCEPTCONN, (char*)&optval, &optlen) != 0) return 0;
    return optval ? 1 : 0;
}

static DWORD WINAPI accept_notifier_thread(LPVOID param) {
    aeApiState *state = (aeApiState *)param;
    aeIocpListen *L = state->listen;
    HANDLE *handles = NULL;
    int *fd_for_handle = NULL;
    int cap = 0;

    while (InterlockedCompareExchange(&L->stop, 0, 0) == 0) {
        EnterCriticalSection(&L->lock);
        int n = L->count;
        if (n + 1 > cap) {
            HANDLE *p = (HANDLE *)zrealloc(handles, (n + 1) * sizeof(HANDLE));
            int *q = (int *)zrealloc(fd_for_handle, (n + 1) * sizeof(int));
            if (!p || !q) { LeaveCriticalSection(&L->lock); break; }
            handles = p;
            fd_for_handle = q;
            cap = n + 1;
        }
        handles[0] = L->update_event;
        fd_for_handle[0] = -1;
        for (int i = 0; i < n; i++) {
            handles[i + 1] = (HANDLE)L->events[i];
            fd_for_handle[i + 1] = L->fds[i];
        }
        LeaveCriticalSection(&L->lock);

        if (n == 0) {
            DWORD w = WaitForSingleObject(L->update_event, 500);
            if (w == WAIT_OBJECT_0) ResetEvent(L->update_event);
            continue;
        }

        DWORD idx = WaitForMultipleObjects((DWORD)(n + 1), handles, FALSE, 500);
        if (idx == WAIT_FAILED || idx == WAIT_TIMEOUT) continue;
        if (idx == WAIT_OBJECT_0) {
            ResetEvent(L->update_event);
            continue;
        }
        idx -= WAIT_OBJECT_0 + 1; /* which listen event */
        if (idx >= (DWORD)n) continue;

        int fd = fd_for_handle[idx + 1];
        EnterCriticalSection(&L->lock);
        WSAEVENT ev = (idx < (DWORD)L->count && L->fds[idx] == fd) ? L->events[idx] : 0;
        LeaveCriticalSection(&L->lock);
        if (ev) WSAResetEvent(ev);

        PostQueuedCompletionStatus(state->iocpHandle, 0, (ULONG_PTR)fd, NULL);
    }

    if (handles) zfree(handles);
    if (fd_for_handle) zfree(fd_for_handle);
    return 0;
}

static int aeApiCreate(aeEventLoop *eventLoop) {
    if (initializeWSA() == -1) return -1;

    aeApiState *state = zmalloc(sizeof(aeApiState));
    if (!state) return -1;

    state->iocpHandle = CreateIoCompletionPort(INVALID_HANDLE_VALUE, NULL, 0, 0);
    if (state->iocpHandle == NULL) {
        zfree(state);
        return -1;
    }

    state->buffers = zmalloc(sizeof(aeIocpBuffer *) * eventLoop->setsize);
    if (!state->buffers) {
        CloseHandle(state->iocpHandle);
        zfree(state);
        return -1;
    }
    memset(state->buffers, 0, sizeof(aeIocpBuffer *) * eventLoop->setsize);

    state->listen = zmalloc(sizeof(aeIocpListen));
    if (!state->listen) {
        zfree(state->buffers);
        CloseHandle(state->iocpHandle);
        zfree(state);
        return -1;
    }
    aeIocpListen *L = state->listen;
    InitializeCriticalSection(&L->lock);
    L->count = 0;
    L->update_event = CreateEvent(NULL, TRUE, FALSE, NULL);
    L->thread = NULL;
    L->stop = 0;

    eventLoop->apidata = state;
    return 0;
}

static int aeApiResize(aeEventLoop *eventLoop, int setsize) {
    aeApiState *state = eventLoop->apidata;
    state->buffers = zrealloc(state->buffers, sizeof(aeIocpBuffer *) * setsize);
    return state->buffers ? 0 : -1;
}

static void aeApiFree(aeEventLoop *eventLoop) {
    aeApiState *state = eventLoop->apidata;
    aeIocpListen *L = state->listen;

    /* Stop accept thread */
    InterlockedExchange(&L->stop, 1);
    SetEvent(L->update_event);
    if (L->thread) {
        WaitForSingleObject(L->thread, 5000);
        CloseHandle(L->thread);
        L->thread = NULL;
    }
    for (int i = 0; i < L->count; i++) {
        WSACloseEvent(L->events[i]);
    }
    CloseHandle(L->update_event);
    DeleteCriticalSection(&L->lock);
    zfree(L);
    state->listen = NULL;

    for (int i = 0; i < eventLoop->setsize; i++) {
        if (state->buffers[i]) {
            aeIocpBuffer *buf = state->buffers[i];
            if (buf->pending_buf) {
                zfree(buf->pending_buf);
                buf->pending_buf = NULL;
            }
            if (buf->fd != INVALID_SOCKET)
                closesocket(buf->fd);
            zfree(buf);
            state->buffers[i] = NULL;
        }
    }
    zfree(state->buffers);
    CloseHandle(state->iocpHandle);
    zfree(state);
    cleanupWSA();
}

static int add_listen_to_notifier(aeApiState *state, int fd) {
    aeIocpListen *L = state->listen;
    SOCKET sock = (SOCKET)fd;
    WSAEVENT ev = WSACreateEvent();
    if (ev == WSA_INVALID_EVENT) return -1;
    if (WSAEventSelect(sock, ev, FD_ACCEPT) == SOCKET_ERROR) {
        WSACloseEvent(ev);
        return -1;
    }
    EnterCriticalSection(&L->lock);
    if (L->count >= MAX_LISTEN_SOCKETS) {
        LeaveCriticalSection(&L->lock);
        WSAEventSelect(sock, 0, 0);
        WSACloseEvent(ev);
        return -1;
    }
    L->fds[L->count] = fd;
    L->events[L->count] = ev;
    L->count++;
    SetEvent(L->update_event);
    if (!L->thread) {
        L->thread = CreateThread(NULL, 0, accept_notifier_thread, state, 0, NULL);
        if (!L->thread) {
            L->count--;
            LeaveCriticalSection(&L->lock);
            WSACloseEvent(ev);
            return -1;
        }
    }
    LeaveCriticalSection(&L->lock);
    return 0;
}

/* Returns 1 if fd was in listen list and removed, 0 otherwise. */
static int remove_listen_from_notifier(aeApiState *state, int fd) {
    aeIocpListen *L = state->listen;
    int removed = 0;
    EnterCriticalSection(&L->lock);
    for (int i = 0; i < L->count; i++) {
        if (L->fds[i] == fd) {
            WSAEVENT ev = L->events[i];
            WSAEventSelect((SOCKET)fd, 0, 0);
            WSACloseEvent(ev);
            L->count--;
            if (i < L->count) {
                L->fds[i] = L->fds[L->count];
                L->events[i] = L->events[L->count];
            }
            SetEvent(L->update_event);
            removed = 1;
            break;
        }
    }
    LeaveCriticalSection(&L->lock);
    return removed;
}

static int aeApiAddEvent(aeEventLoop *eventLoop, int fd, int mask) {
    aeApiState *state = eventLoop->apidata;
    SOCKET sock = (SOCKET)fd;
    int old_mask = eventLoop->events[fd].mask;

    /* Merge with existing mask (same as epoll) */
    mask |= old_mask;

    /* Associate socket with IOCP (for data sockets; listen also needs it for key) */
    if (CreateIoCompletionPort((HANDLE)sock, state->iocpHandle, (ULONG_PTR)fd, 0) == NULL) {
        if (GetLastError() != ERROR_ALREADY_ASSIGNED && GetLastError() != ERROR_INVALID_PARAMETER)
            return -1;
    }

    if (is_listen_socket(sock)) {
        return add_listen_to_notifier(state, fd);
    }

    /* Data socket: ensure buffer exists and post read/write as needed */
    if (!state->buffers[fd]) {
        state->buffers[fd] = zmalloc(sizeof(aeIocpBuffer));
        if (!state->buffers[fd]) return -1;
        memset(state->buffers[fd], 0, sizeof(aeIocpBuffer));
        state->buffers[fd]->fd = sock;
        state->buffers[fd]->read_wsaBuf.buf = state->buffers[fd]->read_buffer;
        state->buffers[fd]->read_wsaBuf.len = MAX_BUFFER_SIZE;
        state->buffers[fd]->write_wsaBuf.buf = state->buffers[fd]->write_buffer;
        state->buffers[fd]->write_wsaBuf.len = 0; /* 0-byte WSASend for writable notification without sending data */
    }

    aeIocpBuffer *buf = state->buffers[fd];
    DWORD bytes = 0, flags = 0;

    /* Only post IO for newly added mask bits (avoid double-posting when adding WRITABLE after READABLE) */
    if ((mask & AE_READABLE) && !(old_mask & AE_READABLE)) {
        memset(&buf->read_overlapped, 0, sizeof(WSAOVERLAPPED));
        buf->read_overlapped.hEvent = IOCP_OP_READ;
        if (WSARecv(sock, &buf->read_wsaBuf, 1, &bytes, &flags, &buf->read_overlapped, NULL) == SOCKET_ERROR) {
            int err = WSAGetLastError();
            /* Allow WSA_IO_PENDING (normal async), WSAEWOULDBLOCK, and WSAENOTCONN
             * (socket still connecting after non-blocking connect). The read will be
             * re-posted once the connection completes via the WRITABLE path in Poll. */
            if (err != WSA_IO_PENDING && err != WSAEWOULDBLOCK && err != WSAENOTCONN)
                return -1;
        }
    }
    if ((mask & AE_WRITABLE) && !(old_mask & AE_WRITABLE)) {
        /* Post a synthetic writable completion directly to IOCP.
         * Carry current write_gen in 'bytes' so poll can detect stale ones. */
        buf->write_gen++;
        memset(&buf->write_overlapped, 0, sizeof(WSAOVERLAPPED));
        buf->write_overlapped.hEvent = IOCP_OP_WRITE;
        PostQueuedCompletionStatus(state->iocpHandle, buf->write_gen, (ULONG_PTR)fd, (LPOVERLAPPED)&buf->write_overlapped);
    }
    return 0;
}

static void aeApiDelEvent(aeEventLoop *eventLoop, int fd, int delmask) {
    aeApiState *state = eventLoop->apidata;
    /* ae.c calls aeApiDelEvent BEFORE updating fe->mask, so compute remaining ourselves */
    int remaining = eventLoop->events[fd].mask & (~delmask);

    /* Listen sockets are not in buffers[]; remove from notifier only. */
    if (remove_listen_from_notifier(state, fd)) {
        return;
    }

    if (state->buffers[fd]) {
        aeIocpBuffer *buf = state->buffers[fd];
        SOCKET sock = buf->fd;
        if (delmask & AE_READABLE)
            CancelIoEx((HANDLE)sock, (LPOVERLAPPED)&buf->read_overlapped);
        if (delmask & AE_WRITABLE)
            buf->write_gen++;  /* Invalidate any pending synthetic WRITABLE completion */

        if (remaining == AE_NONE) {
            /* Cannot free buffer now -- IOCP queue may still hold completions
             * referencing it (read overlapped, or stale write completions).
             * Cancel pending read IO, invalidate write gen, and mark as
             * closing.  The buffer will be freed when poll drains the stale
             * completions.  The caller is responsible for closing the socket. */
            CancelIoEx((HANDLE)sock, NULL);  /* cancel ALL pending IO on this socket */
            buf->closing = 1;
            buf->write_gen++;
            if (buf->pending_buf) {
                zfree(buf->pending_buf);
                buf->pending_buf = NULL;
            }
            return;
        }

        /* Re-post remaining direction(s) */
        DWORD bytes = 0, flags = 0;
        if (remaining & AE_READABLE) {
            memset(&buf->read_overlapped, 0, sizeof(WSAOVERLAPPED));
            buf->read_overlapped.hEvent = IOCP_OP_READ;
            WSARecv(sock, &buf->read_wsaBuf, 1, &bytes, &flags, &buf->read_overlapped, NULL);
        }
        if (remaining & AE_WRITABLE) {
            buf->write_gen++;
            memset(&buf->write_overlapped, 0, sizeof(WSAOVERLAPPED));
            buf->write_overlapped.hEvent = IOCP_OP_WRITE;
            PostQueuedCompletionStatus(state->iocpHandle, buf->write_gen, (ULONG_PTR)fd, (LPOVERLAPPED)&buf->write_overlapped);
        }
    }
}

static int aeApiPoll(aeEventLoop *eventLoop, struct timeval *tvp) {
    aeApiState *state = eventLoop->apidata;
    int numevents = 0;
    DWORD timeout = tvp ? (DWORD)(tvp->tv_sec * 1000 + tvp->tv_usec / 1000) : INFINITE;

    while (numevents < eventLoop->setsize) {
        DWORD bytes = 0;
        ULONG_PTR key = 0;
        OVERLAPPED *overlapped = NULL;

        int ret = GetQueuedCompletionStatus(state->iocpHandle, &bytes, &key, &overlapped, timeout);
        /* After first successful dequeue, switch to non-blocking poll for remaining events */
        timeout = 0;

        if (!ret) {
            if (GetLastError() == WAIT_TIMEOUT) break;
            if (overlapped) {
                DWORD err = GetLastError();
                int fd = (int)key;
                aeIocpBuffer *buf = state->buffers[fd];
                /* Drain closing buffers or cancelled IO silently */
                if (!buf || buf->closing) {
                    if (buf) { zfree(buf); state->buffers[fd] = NULL; }
                    continue;
                }
                if (err == ERROR_OPERATION_ABORTED) {
                    continue;
                }
                eventLoop->fired[numevents].fd = fd;
                eventLoop->fired[numevents].mask = AE_READABLE;
                numevents++;
            }
            continue;
        }

        /* overlapped == NULL: listen fd ready (posted by accept thread) */
        if (!overlapped) {
            int fd = (int)key;
            eventLoop->fired[numevents].fd = fd;
            eventLoop->fired[numevents].mask = AE_READABLE;
            numevents++;
            continue;
        }

        int fd = (int)key;
        aeIocpBuffer *buf = state->buffers[fd];
        if (!buf) continue;

        /* Buffer marked for deferred cleanup -- free it now that the
         * completion has been drained from the IOCP queue. */
        if (buf->closing) {
            zfree(buf);
            state->buffers[fd] = NULL;
            continue;
        }

        int mask = 0;
        if (overlapped == (OVERLAPPED *)&buf->read_overlapped) {
            mask = AE_READABLE;
        } else if (overlapped == (OVERLAPPED *)&buf->write_overlapped) {
            /* Synthetic WRITABLE: validate generation to discard stale completions.
             * PostQueuedCompletionStatus carried write_gen in 'bytes'. */
            if (bytes == buf->write_gen)
                mask = AE_WRITABLE;
            else
                continue;  /* stale completion -- skip */
        }
        if (mask) {
            eventLoop->fired[numevents].fd = fd;
            eventLoop->fired[numevents].mask = mask;
            numevents++;
        }

        /* For READABLE: save received data so application read() can get it (IOCP already consumed it) */
        if (mask == AE_READABLE && bytes > 0 && buf->pending_buf == NULL) {
            buf->pending_buf = zmalloc((size_t)bytes);
            if (buf->pending_buf) {
                memcpy(buf->pending_buf, buf->read_buffer, (size_t)bytes);
                buf->pending_len = (size_t)bytes;
                buf->pending_off = 0;
            }
        }

        /* Re-post if still wanted (level-triggered); for read, re-post after app consumes pending */
        int want = eventLoop->events[fd].mask;
        if (mask == AE_READABLE && (want & AE_READABLE) && buf->pending_buf == NULL) {
            memset(&buf->read_overlapped, 0, sizeof(WSAOVERLAPPED));
            buf->read_overlapped.hEvent = IOCP_OP_READ;
            DWORD flags = 0;
            WSARecv(buf->fd, &buf->read_wsaBuf, 1, &bytes, &flags, &buf->read_overlapped, NULL);
        }
        /* When we get READABLE and fd wants WRITABLE, try posting write (e.g. connect just completed) */
        if (mask == AE_READABLE && (want & AE_WRITABLE)) {
            buf->write_gen++;
            memset(&buf->write_overlapped, 0, sizeof(WSAOVERLAPPED));
            buf->write_overlapped.hEvent = IOCP_OP_WRITE;
            PostQueuedCompletionStatus(state->iocpHandle, buf->write_gen, (ULONG_PTR)fd, (LPOVERLAPPED)&buf->write_overlapped);
        }
        /* Do NOT re-post WRITABLE automatically after completion.
         * The application callback will send data via write()/send() and then
         * either delete WRITABLE or re-register it via aeCreateFileEvent,
         * which calls aeApiAddEvent to post a new WSASend. */
    }
    return numevents;
}

/* Called when application does read()/recv(): return data we already received via WSARecv. */
int aeApiTakePendingRead(aeEventLoop *eventLoop, int fd, void *buf, size_t len) {
    aeApiState *state = eventLoop->apidata;
    if (fd < 0 || fd >= eventLoop->setsize || !state->buffers[fd] || !state->buffers[fd]->pending_buf)
        return -1;
    aeIocpBuffer *b = state->buffers[fd];
    size_t avail = b->pending_len - b->pending_off;
    if (avail == 0) return -1;
    size_t tocopy = len < avail ? len : avail;
    memcpy(buf, b->pending_buf + b->pending_off, tocopy);
    b->pending_off += tocopy;
    if (b->pending_off >= b->pending_len) {
        zfree(b->pending_buf);
        b->pending_buf = NULL;
        b->pending_len = b->pending_off = 0;
        /* Re-post WSARecv for next batch */
        if (eventLoop->events[fd].mask & AE_READABLE) {
            DWORD bytes = 0, flags = 0;
            memset(&b->read_overlapped, 0, sizeof(WSAOVERLAPPED));
            b->read_overlapped.hEvent = IOCP_OP_READ;
            WSARecv(b->fd, &b->read_wsaBuf, 1, &bytes, &flags, &b->read_overlapped, NULL);
        }
    }
    return (int)tocopy;
}

static char *aeApiName(void) {
    return "iocp";
}
