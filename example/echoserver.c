#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#ifndef _WIN32
#include <unistd.h>
#endif

#include "../src/ae.h"
#include "../src/anet.h"

long long datalength = 0;

void readFromClient(aeEventLoop *loop, int fd, void *clientdata, int mask);

void writeToClient(aeEventLoop *loop, int fd, void *clientdata, int mask)
{
    char *buffer = clientdata;
    anetWrite(fd, buffer, strlen(buffer));
    free(buffer);
    aeDeleteFileEvent(loop, fd, mask);
}

void readFromClient(aeEventLoop *loop, int fd, void *clientdata, int mask)
{
    int buffer_size = 1024;
    char *buffer = malloc(sizeof(char) * buffer_size);
    memset(buffer, 0x00, sizeof(char) * buffer_size);
    int size =  read(fd, buffer, buffer_size);
    if (size <= 0)
    {
      printf("Client disconnected\n");
      free(buffer);
      aeDeleteFileEvent(loop, fd, AE_READABLE);
      return; 
    }

    //printf("Read from client, %s\n", buffer);
    aeCreateFileEvent(loop, fd, AE_READABLE, readFromClient, NULL);
    aeCreateFileEvent(loop, fd, AE_WRITABLE, writeToClient, buffer);
    datalength += size;
}

void acceptTcpHandler(aeEventLoop *loop, int fd, void *clientdata, int mask)
{
    int client_port, client_fd;
    char client_ip[128];
    // create client socket
    client_fd = anetTcpAccept(NULL, fd, client_ip, 128, &client_port);
    printf("Accepted %s:%d\n", client_ip, client_port);

    // set client socket non-block
    anetNonBlock(NULL, client_fd);

    // regist on message callback
    int ret;
    ret = aeCreateFileEvent(loop, client_fd, AE_READABLE, readFromClient, NULL);
    assert(ret != AE_ERR);
}

int CalcByteTimer(struct aeEventLoop *loop, long long id, void *clientData)
{
    static long long curlength = 0;
    int rate = (datalength - curlength) / (1024 *1024);
    printf("Recive rate: %d MB/s\n", rate);
    curlength = datalength;

    aeCreateTimeEvent(loop, 1000, CalcByteTimer,NULL, NULL);

    return -1;
}

int main()
{
    int ipfd;
	// create main event loop
    aeEventLoop *loop;
    loop = aeCreateEventLoop(1024);

    // create server socket
    ipfd = anetTcpServer(NULL, 8000, "0.0.0.0", 0);
    assert(ipfd != ANET_ERR);
    printf("server listen on 8000\n");

    anetNonBlock(NULL, ipfd);
    anetEnableTcpNoDelay(NULL, ipfd);

    // regist socket connect callback
    int ret;
    ret = aeCreateFileEvent(loop, ipfd, AE_READABLE, acceptTcpHandler, NULL);
    assert(ret != AE_ERR);

    ret = aeCreateTimeEvent(loop, 1000, CalcByteTimer,NULL, NULL);
    assert(ret != AE_ERR);

    // start main loop
    aeMain(loop);

    // stop loop
    aeDeleteEventLoop(loop);

    return 0;
}
