#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#ifndef _WIN32
#include <unistd.h>
#endif

#include "../src/ae.h"
#include "../src/anet.h"

void readFromServer(aeEventLoop *loop, int fd, void *clientdata, int mask);

void writeToServer(aeEventLoop *loop, int fd, void *clientdata, int mask)
{
    char *buffer = calloc(1024,1);
    memset(buffer,'a', 1023);
    int size = anetWrite(fd, buffer, strlen(buffer));
    //printf("write to server %d byte\n", size);
    aeDeleteFileEvent(loop, fd, AE_WRITABLE);
    free(buffer);
}

void readFromServer(aeEventLoop *loop, int fd, void *clientdata, int mask)
{
    int buffer_size = 1024;
    char *buffer = malloc(sizeof(char) * buffer_size);
    memset(buffer, 0x00, sizeof(char) * buffer_size);
    int size;
    size = read(fd, buffer, buffer_size);
    if(size <= 0)
    {
        aeDeleteFileEvent(loop, fd, AE_READABLE);
        aeDeleteFileEvent(loop, fd, AE_WRITABLE);
        close(fd);
    }
    else
    {
        //printf("read from server %d bytes\n", size);
        aeCreateFileEvent(loop, fd, AE_WRITABLE, writeToServer, NULL);
    }
    
    free(buffer);
}

int main()
{
    int ipfd;
	// create main event loop
    aeEventLoop *loop;
    loop = aeCreateEventLoop(65536);
	
    int connected = 0;
    for(int i = 0; i < 100; i++)
    {
        // create connection (blocking connect, then set non-block for I/O)
        ipfd = anetTcpConnect(NULL,"127.0.0.1", 8000);
        if (ipfd == ANET_ERR) {
            printf("connect to server failed, skipping\n");
            continue;
        }
        printf("connect to server fd: %d\n", ipfd);

        anetNonBlock(NULL, ipfd);
        anetEnableTcpNoDelay(NULL, ipfd);

        // regist socket read callback
        int ret;
        ret = aeCreateFileEvent(loop, ipfd, AE_READABLE, readFromServer, NULL);
        assert(ret != AE_ERR);

        // regist socket write callback
        ret = aeCreateFileEvent(loop, ipfd, AE_WRITABLE, writeToServer, NULL);
        assert(ret != AE_ERR);
        connected++;
    }

    if (connected == 0) {
        printf("No connections established, exiting.\n");
        aeDeleteEventLoop(loop);
        return 1;
    }
    printf("%d connections established.\n", connected);

    // start main loop
    aeMain(loop);

    // stop loop
    aeDeleteEventLoop(loop);

    return 0;
}