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
    //printf("write to server %d byte, %s\n", size, buffer);
    aeDeleteFileEvent(loop, fd, mask);
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
        printf("Remote disconnected.\n");
        aeDeleteFileEvent(loop, fd, AE_READABLE);
    }
    else
    {
        //printf("read from server, %s\n", buffer);
        aeCreateFileEvent(loop, fd, AE_WRITABLE, writeToServer, NULL);
        aeCreateFileEvent(loop, fd, AE_READABLE, readFromServer, NULL);
    }
    
    free(buffer);
}

int main()
{
    int ipfd;
	// create main event loop
    aeEventLoop *loop;
    loop = aeCreateEventLoop(40960);
	
    for(int i = 0; i < 1024; i++)
    {
        // create connection
        ipfd = anetTcpNonBlockConnect(NULL,"172.16.40.125", 8000);
        assert(ipfd != ANET_ERR);

        anetNonBlock(NULL, ipfd);
        anetEnableTcpNoDelay(NULL, ipfd);

        // regist socket read callback
        int ret;
        ret = aeCreateFileEvent(loop, ipfd, AE_READABLE, readFromServer, NULL);
        assert(ret != AE_ERR);

        // regist socket write callback
        ret = aeCreateFileEvent(loop, ipfd, AE_WRITABLE, writeToServer, NULL);
        assert(ret != AE_ERR);
    }

    // start main loop
    aeMain(loop);

    // stop loop
    aeDeleteEventLoop(loop);

    return 0;
}