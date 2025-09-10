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

// Global WSA initialization flag
static int wsaInitialized = 0;

// Initialize WSA for Windows
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

// Cleanup WSA
static void cleanupWSA(void) {
    if (wsaInitialized) {
        WSACleanup();
        wsaInitialized = 0;
    }
}

typedef struct aeIocpBuffer {
    WSAOVERLAPPED overlapped;
    WSABUF wsaBuf;
    char buffer[MAX_BUFFER_SIZE];
    int operation;  // 0 for read, 1 for write
    SOCKET fd;      // Store the socket handle
} aeIocpBuffer;

typedef struct aeApiState {
    HANDLE iocpHandle;
    aeIocpBuffer **buffers;  // Array of buffer pointers for each fd
} aeApiState;

static int aeApiCreate(aeEventLoop *eventLoop) {
    // Initialize WSA first
    if (initializeWSA() == -1) {
        return -1;
    }

    aeApiState *state = zmalloc(sizeof(aeApiState));
    if (!state) return -1;

    // Create IOCP handle
    state->iocpHandle = CreateIoCompletionPort(INVALID_HANDLE_VALUE, NULL, 0, 0);
    if (state->iocpHandle == NULL) {
        zfree(state);
        return -1;
    }

    // Allocate buffer pointers array
    state->buffers = zmalloc(sizeof(aeIocpBuffer*) * eventLoop->setsize);
    if (!state->buffers) {
        CloseHandle(state->iocpHandle);
        zfree(state);
        return -1;
    }
    memset(state->buffers, 0, sizeof(aeIocpBuffer*) * eventLoop->setsize);

    eventLoop->apidata = state;
    return 0;
}

static int aeApiResize(aeEventLoop *eventLoop, int setsize) {
    aeApiState *state = eventLoop->apidata;
    state->buffers = zrealloc(state->buffers, sizeof(aeIocpBuffer*) * setsize);
    return 0;
}

static void aeApiFree(aeEventLoop *eventLoop) {
    aeApiState *state = eventLoop->apidata;
    
    // Clean up buffers
    for (int i = 0; i < eventLoop->setsize; i++) {
        if (state->buffers[i]) {
            // Close socket if still open
            if (state->buffers[i]->fd != INVALID_SOCKET) {
                closesocket(state->buffers[i]->fd);
            }
            zfree(state->buffers[i]);
        }
    }
    
    zfree(state->buffers);
    CloseHandle(state->iocpHandle);
    zfree(state);

    // Cleanup WSA when the last event loop is freed
    cleanupWSA();
}

static int aeApiAddEvent(aeEventLoop *eventLoop, int fd, int mask) {
    aeApiState *state = eventLoop->apidata;
    SOCKET sock = (SOCKET)fd;
    
    // Create buffer if not exists
    if (!state->buffers[fd]) {
        state->buffers[fd] = zmalloc(sizeof(aeIocpBuffer));
        if (!state->buffers[fd]) return -1;
        
        memset(state->buffers[fd], 0, sizeof(aeIocpBuffer));
        state->buffers[fd]->wsaBuf.buf = state->buffers[fd]->buffer;
        state->buffers[fd]->wsaBuf.len = MAX_BUFFER_SIZE;
        state->buffers[fd]->fd = sock;
    }

    // Associate socket with IOCP
    if (CreateIoCompletionPort((HANDLE)sock, state->iocpHandle, (ULONG_PTR)fd, 0) == NULL) {
        if (GetLastError() != ERROR_INVALID_PARAMETER) { // Skip error if socket already associated
            // Don't return error here, as the socket might be in connecting state
            // Just continue with the operation
        }
    }

    // For listening socket, we only need to associate it with IOCP
    // No need to start async operations
    SOCKADDR addr;
    int addrlen = sizeof(addr);
    if (getsockname(sock, &addr, &addrlen) == 0 && 
        (addr.sa_family == AF_INET || addr.sa_family == AF_INET6)) {
        int optval;
        int optlen = sizeof(optval);
        if (getsockopt(sock, SOL_SOCKET, SO_ACCEPTCONN, (char*)&optval, &optlen) == 0 && optval) {
            // This is a listening socket
            return 0;
        }
    }

    // For normal sockets, start async operations
    DWORD flags = 0;
    DWORD bytes;
    aeIocpBuffer *buffer = state->buffers[fd];

    if (mask & AE_READABLE) {
        buffer->operation = 0;  // read
        memset(&buffer->overlapped, 0, sizeof(WSAOVERLAPPED));
        if (WSARecv(sock, &buffer->wsaBuf, 1, &bytes, &flags,
                    &buffer->overlapped, NULL) == SOCKET_ERROR) {
            int err = WSAGetLastError();
            if (err != WSA_IO_PENDING && err != WSAEWOULDBLOCK) {
                // Don't fail on WSAEWOULDBLOCK, it's normal for non-blocking sockets
                return -1;
            }
        }
    }
    
    if (mask & AE_WRITABLE) {
        buffer->operation = 1;  // write
        memset(&buffer->overlapped, 0, sizeof(WSAOVERLAPPED));
        if (WSASend(sock, &buffer->wsaBuf, 1, &bytes, 0,
                    &buffer->overlapped, NULL) == SOCKET_ERROR) {
            int err = WSAGetLastError();
            if (err != WSA_IO_PENDING && err != WSAEWOULDBLOCK) {
                // Don't fail on WSAEWOULDBLOCK, it's normal for non-blocking sockets
                return -1;
            }
        }
    }

    return 0;
}

static void aeApiDelEvent(aeEventLoop *eventLoop, int fd, int mask) {
    aeApiState *state = eventLoop->apidata;
    
    if (state->buffers[fd]) {
        // Check if this is a listening socket
        SOCKET sock = state->buffers[fd]->fd;
        SOCKADDR addr;
        int addrlen = sizeof(addr);
        if (getsockname(sock, &addr, &addrlen) == 0 && 
            (addr.sa_family == AF_INET || addr.sa_family == AF_INET6)) {
            int optval;
            int optlen = sizeof(optval);
            if (getsockopt(sock, SOL_SOCKET, SO_ACCEPTCONN, (char*)&optval, &optlen) == 0 && optval) {
                // For listening socket, no need to cancel IO
                if (!(eventLoop->events[fd].mask & ~mask)) {
                    zfree(state->buffers[fd]);
                    state->buffers[fd] = NULL;
                }
                return;
            }
        }

        // Cancel pending operations for normal sockets
        CancelIoEx((HANDLE)state->buffers[fd]->fd, &state->buffers[fd]->overlapped);
        
        if (!(eventLoop->events[fd].mask & ~mask)) {
            zfree(state->buffers[fd]);
            state->buffers[fd] = NULL;
        }
    }
}

static int aeApiPoll(aeEventLoop *eventLoop, struct timeval *tvp) {
    aeApiState *state = eventLoop->apidata;
    int numevents = 0;
    DWORD timeout = tvp ? (tvp->tv_sec * 1000 + tvp->tv_usec / 1000) : INFINITE;
    
    while (numevents < eventLoop->setsize) {
        DWORD bytes;
        ULONG_PTR key;
        OVERLAPPED *overlapped;
        
        int ret = GetQueuedCompletionStatus(state->iocpHandle, &bytes, &key,
                                          &overlapped, timeout);
        
        // Only process first event if timeout specified
        if (tvp) timeout = 0;
        
        if (!ret) {
            DWORD error = GetLastError();
            if (error == WAIT_TIMEOUT) break;
            
            // Handle disconnection
            if (overlapped) {
                int fd = (int)key;
                aeIocpBuffer *buffer = state->buffers[fd];
                if (buffer) {
                    eventLoop->fired[numevents].fd = fd;
                    eventLoop->fired[numevents].mask = AE_READABLE; // Fire read event to let application handle disconnection
                    numevents++;
                }
            }
            continue;
        }

        if (overlapped) {
            int fd = (int)key;
            aeIocpBuffer *buffer = state->buffers[fd];
            
            if (buffer) {
                int mask = 0;
                if (buffer->operation == 0) {  // read
                    mask |= AE_READABLE;
                } else {  // write
                    mask |= AE_WRITABLE;
                }
                
                if (mask) {
                    eventLoop->fired[numevents].fd = fd;
                    eventLoop->fired[numevents].mask = mask;
                    numevents++;
                }
                
                // Repost read operation
                if (buffer->operation == 0 && eventLoop->events[fd].mask & AE_READABLE) {
                    DWORD flags = 0;
                    memset(&buffer->overlapped, 0, sizeof(WSAOVERLAPPED));
                    WSARecv(buffer->fd, &buffer->wsaBuf, 1, &bytes, &flags,
                           &buffer->overlapped, NULL);
                }
            }
        }
    }
    
    return numevents;
}

static char *aeApiName(void) {
    return "iocp";
} 