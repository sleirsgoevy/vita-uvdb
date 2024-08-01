#pragma once

//uvdb_enter acts as a software breakpoint. on first hit, the program will wait for GDB to connect. on subsequent hits, it will simply act as a software breakpoint
void uvdb_enter(void);
