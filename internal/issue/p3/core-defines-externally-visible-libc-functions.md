# Core stdio bodies interpose on C and C++ objects

**RULED 2026-10-21:** CFlat definitions of libc names are marked `nobuiltin`, so LLVM calls the
CFlat body; users may still override libc names. This ruling does not rename core bodies or make
them internal.

`cflat/core/cruntime.cb` defines 19 C-linkage bodies for hook-aware stdio: printf, sprintf,
snprintf, vprintf, vsprintf, vsnprintf, puts, putchar, putn, fputs, fputc, putc, fprintf, fgets,
scanf, getchar, getc, sscanf, and fscanf.

A separately compiled `.c` object or a C++ header inline that calls `printf` still resolves to the
core definition instead of host libc. This object-file interposition remains outside the T444
ruling.

T444: nobuiltin landed; C-object interposition unchanged
