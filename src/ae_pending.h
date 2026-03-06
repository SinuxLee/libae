#ifndef AE_PENDING_H
#define AE_PENDING_H

#include <stddef.h>

struct aeEventLoop;

/* Used by sockcompat win32_recv to return data already received by IOCP. */
struct aeEventLoop *aeGetCurrentEventLoop(void);
int aeTakePendingRead(struct aeEventLoop *eventLoop, int fd, void *buf, size_t len);

#endif
