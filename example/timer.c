#include <stdlib.h>
#include <stdio.h>
#include <time.h>

#include "../src/ae.h"

#define BUFF_SIZE 512
#define TIMER_SIZE 10

void freeClientData(struct aeEventLoop *eventLoop, void *clientData)
{
    if(NULL != clientData)
        free(clientData);
}

int print(struct aeEventLoop *loop, long long id, void *clientData)
{
    printf("event %lld - %s\n", id, (const char *)clientData);
    if (id == TIMER_SIZE)
    {
        aeStop(loop);
    }
    
    return AE_NOMORE;
}

int main(void)
{
    aeEventLoop *loop = aeCreateEventLoop(TIMER_SIZE);
    int i;
    for (i = 0; i <= TIMER_SIZE; i ++) {
        char *eventData = calloc(BUFF_SIZE, sizeof(char));
        if (NULL != eventData)
        {
            sprintf(eventData, "Hello World %d", i);
            aeCreateTimeEvent(loop, i*1000, print, eventData, freeClientData);
        }
    }
    aeMain(loop);
    aeDeleteEventLoop(loop);
    return 0;
}
