#include <stdlib.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/socket.h>
#include <psp2/kernel/threadmgr/lw_mutex.h>
#include "uvdb.h"

//this code has to use posix apis, because that's what homebrew uses
//the native stdout/stderr handles are completely ignored by newlib
//thankfully, signal safety is not an issue here

//unfortunately, newlib does not export dup2. this is a hack, but probably relatively safe one
//would be nice if newlib supported "custom" fds with user-provided vtables, this way we could avoid threading at all
static int my_dup2(int fd1, int fd2)
{
    extern void* __vita_fdmap[];
    extern SceKernelLwMutexWork _newlib_fd_mutex;
    int fd1_dup = dup(fd1);
    if(fd1_dup < 0)
        return fd1_dup;
    sceKernelLockLwMutex(&_newlib_fd_mutex, 1, 0);
    void* a = __vita_fdmap[fd1_dup];
    void* b = __vita_fdmap[fd2];
    __vita_fdmap[fd1_dup] = b;
    __vita_fdmap[fd2] = a;
    sceKernelUnlockLwMutex(&_newlib_fd_mutex, 1);
    if(close(fd1_dup))
        return -1;
    return fd2;
}

static void* redir_thread(void* arg)
{
    int source_pipe = (int)arg;
    char buf[1024];
    ssize_t chk;
    for(;;)
    {
        ssize_t chk = read(source_pipe, buf, sizeof(buf));
        if(chk > 0)
        {
            size_t pos = 0;
            while(pos < chk)
            {
                ssize_t chk2 = uvdb_remote_syscall("write", 3, 1, buf, chk);
                if(chk2 <= 0)
                    break;
                pos += chk2;
            }
        }
    }
    return 0;
}

int uvdb_redirect_stdio(void)
{
    int pp[2];
    if(socketpair(AF_INET, SOCK_STREAM, 0, pp) && pipe(pp))
        return -1;
    pthread_t pth;
    if(pthread_create(&pth, 0, redir_thread, (void*)pp[1]))
        return -1;
    if(my_dup2(pp[0], 1) != 1 || my_dup2(pp[0], 2) != 2)
        return -1;
    return 0;
}
