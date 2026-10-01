# Core defines externally visible C library functions (printf, puts, scanf, ...)

Ruling 2026-09-30 (maintainer): core must not define externally visible functions; a USER program may
define a C library name (C linkage) and it must win (see p2/o2-builtin-folding item 1).

Today cflat/core/cruntime.cb defines 19 C-linkage bodies that interpose libc for the whole link:
printf, sprintf, snprintf, vprintf, vsprintf, vsnprintf, puts, putchar, putn, fputs, fputc, putc,
fprintf, fgets, scanf, getchar, getc, sscanf, fscanf (`extern int printf(...) { ... }`, ~191-486).
They are the hook-aware / VT-correct stdio used by CFlat code (stdout capture for --run and
program.onStdout, per the comments).

Consequences: a linked `.c` object or C++ import calling printf lands in core's body; a user program
defining its own printf collides with core's definition instead of simply winning.

Fix direction: give the core bodies internal linkage (or a `__cflat_` name) and bind CFlat-source calls
of these names to them; C/C++ objects then get real libc. Decide first whether C/C++ code must keep
the stdout hook under --run (today it does by accident of interposition) - that is the one behaviour
this changes. Sweep core for any other C-linkage body.
