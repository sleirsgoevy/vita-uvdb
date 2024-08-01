# uvdb

This is an intra-process GDB stub for the PSVita that should (for the most part) "just work".

The reason I created this is because [kvdb](https://github.com/DaveeFTW/kvdb) only supports firmware 3.60 (I'm using 3.65), lists hilarious bugs on its README, and is apparently not that easy to setup. On contrary, uvdb contains no kernel-mode code and uses a well-established kernel plugin (kubridge) for the heavy lifting, and thus should not cause as many stability issues.

## Installation

Add [kubridge](https://github.com/bythos14/kubridge/releases) to your PSVita's ur0:tai/config.txt.

Replace `$VITASDK/arm-vita-eabi/kubridge.h` with the one from the above-linked repository.

Type `make` to build.

(Optionally, but this will make your life easier) Copy `uvdb.h` to `$VITASDK/arm-vita-eabi/include`, and `libuvdb.a` to `$VITASDK/arm-vita-eabi/lib`.

## Usage

Link your project with `libuvdb.a` (`-luvdb` linker flag), then call `uvdb_enter()` wherever in your code you need a software breakpoint.

Then run `arm-vita-eabi-gdb program.elf -ex 'target remote PS.VITA.IP.ADDR:1234'` to connect GDB to the console and start debugging.

Working features:

* Memory reading/writing
* Breakpoints
* Single-stepping
* ASLR defeat (base address of the program is resolved)

Non-working features:

* Watchpoints
* `catch syscall`
* Resolving base addresses of libraries (most Vita homebrew is single-binary anyway)
* Probably anything else...

## Bugs and caveats

Unlike x86, ARM does not support proper single-stepping and hardware breakpoints. To overcome this, GDB parses the instruction itself and sets temporary (software) breakpoints at all possible branch targets. This is fine, albeit slow (unfortunately, GDB's serial debugging protocol was never designed to run over TCP), in single-threaded programs, but in multi-threaded programs you may miss some breakpoints.

uvdb uses kubridge's exception handling feature to catch exceptions. If your homebrew installs its own exception handler, you will have problems. If you want to do so, register them *after* `uvdb_enter()` has been called at least once, and save and call the original handler once you determine that you can't handle the exception.

Also (obviously?) uvdb does not work in kernel mode, thus you can't use it to debug kernel plugins.

Anything else? Feel free to [file a bug report](https://github.com/sleirsgoevy/vita-uvdb/issues/new).
