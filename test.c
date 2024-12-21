#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#include <time.h>
#include "debugScreen.h"
#include "uvdb.h"

int main(void)
{
    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in sin = {
        .sin_family = AF_INET,
        .sin_addr = {.s_addr = htonl(0x08080808)},
        .sin_port = htons(53),
    };
    connect(sock, (void*)&sin, sizeof(sin));
    socklen_t l = sizeof(sin);
    getsockname(sock, (void*)&sin, &l);
    close(sock);
    psvDebugScreenInit();
    uint8_t addr[4];
    memcpy(addr, &sin.sin_addr.s_addr, 4);
    psvDebugScreenPrintf("Run the following command on your PC:\n");
    psvDebugScreenPrintf("$ gdb test.elf -ex 'target remote %hhu.%hhu.%hhu.%hhu:1234'\n", addr[0], addr[1], addr[2], addr[3]);
    uvdb_enter();
    uvdb_redirect_stdio();
    //let's do something fun and run fizzbuzz
    for(int i = 1;; i++)
    {
        if(i % 6 == 0)
        {
            printf("FizzBuzz\n");
            psvDebugScreenPrintf("FizzBuzz\n");
        }
        else if(i % 2 == 0)
        {
            printf("Fizz\n");
            psvDebugScreenPrintf("Fizz\n");
        }
        else if(i % 3 == 0)
        {
            printf("Buzz\n");
            psvDebugScreenPrintf("Buzz\n");
        }
        else
        {
            printf("%d\n", i);
            psvDebugScreenPrintf("%d\n", i);
        }
    }
    usleep(3000000);
    return 0;
}
