#pragma once

//uvdb_enter acts as a software breakpoint. on first hit, the program will wait for GDB to connect. on subsequent hits, it will simply act as a software breakpoint
void uvdb_enter(void);

//gdb exposes a remote syscall api to call some (whitelisted) syscalls on the host
//example:
//  uvdb_remote_syscall("write", 3, 1, "Hello, world!\n", 14); //prints hello world in the debugger prompt
int uvdb_remote_syscall(const char* name, int nargs, ... /* int arg1, int arg2, ... */);

//redirects stdout/stderr to go through uvdb_remote_syscall. useful to avoid princesslog & friends
//note: this uses newlib apis, not sce ones
int uvdb_redirect_stdio(void);
