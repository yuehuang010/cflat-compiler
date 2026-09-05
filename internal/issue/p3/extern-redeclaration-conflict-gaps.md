# Extern redeclaration conflicts: two silent drops and a weak message

Bucket: batch mode (diagnostics; one adds a rejection). Filed 2026-09-04 from the q11 review
(d1952097), which made a repeat `extern` declaration under a core name with a DIFFERENT llvm
function type a hard "conflicting declaration" error at the extern line.

## Remaining gaps

1. The `.c` / header-binding route (`import "util.c"` auto-extern, `import package "x.h"`) still
   silently drops a prototype that collides with a hand-written or core declaration. Documented as
   "hand-written wins"; a colliding C prototype should at least be diagnosed when the llvm type
   differs.
2. A same-lowering respelling (`extern int f(u32)` vs core `f(int)`) is still silently dropped:
   the repeat-declaration check compares llvm `FunctionType`, so CFlat-level type differences
   that lower identically are invisible. Decide whether the CFlat signature must match too.
3. The conflicting-declaration message never states the EXISTING signature, and its file-I/O
   tail is baked into a live locale key (the same `LogError` string serves the file-import
   conflict), so a wording fix touches a translated entry.

## Fix direction

Route the header/`.c` auto-extern registration through the same `CreateFunctionDeclaration`
repeat check (cflat/LLVMBackend_ControlFlowAndFunctions.cpp ~1295); compare the recorded CFlat
parameter spellings, not only the llvm type, for hand-written repeats; split the message so the
existing signature is printed. Accept-set: every `extern` in cflat/core/*.cb and Test/test_c*.cb,
plus the header-binding tests (Windows-only skips on macOS - say what you could not run).
