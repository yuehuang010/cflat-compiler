# Extern redeclaration: a same-lowering respelling is still silently dropped

Bucket: batch mode (diagnostics). Filed 2026-09-04 from the q11 review (d1952097), which made a
repeat `extern` declaration under a core name with a DIFFERENT llvm function type a hard
"conflicting declaration" error. Narrowed 2026-09-05: the other two gaps that file carried are
landed, this one needs a maintainer ruling before anything is built.

## The gap

A same-lowering respelling is silently dropped. `extern int f(u32)` next to core's `f(int)` is
accepted as a repeat declaration and the core one binds, because the repeat-declaration check in
`CreateFunctionDeclaration` (cflat/LLVMBackend_ControlFlowAndFunctions.cpp, the
`existing->getFunctionType() == functionType` arm) compares the llvm `FunctionType`. CFlat-level
type differences that lower identically - signedness, a type alias, `u32` vs `int` - are
invisible to it, so the declared spelling is never scored at a call site.

This is the same class of hole the q11 change closed for DIFFERING lowerings: there, `extern void
exit(u8 c)` bypassed the no-implicit-narrowing ruling by spelling. Here the bypass survives
whenever the two spellings happen to share a lowering.

## Needs a ruling before any fix

Does the CFlat signature have to match, or only the lowered one? Both answers are defensible and
the choice decides the size of the change:

- **Only the lowering matters** (today's behaviour): close as working-as-intended. Signedness and
  aliases are then explicitly not part of a linkage name's identity.
- **The CFlat spelling must match too**: the check has to compare recorded parameter spellings,
  not just the llvm type. That is a REJECTION being widened, so it needs its own accept-set
  first. The known hazard is C interop, where the header route legitimately respells core types
  (`DWORD` for `i32`, `unsigned long` for `u32`, `size_t` for `u64`) - `import "windows.h"` and
  the grouped `import { "windows.h", "tlhelp32.h" };` currently compile precisely because those
  respellings lower identically, and a spelling comparison would reject them wholesale. Any fix
  has to exempt the C-import route or normalise its spellings first.

## What is already landed (do not re-file)

- The `.c` / header route does NOT silently drop a colliding prototype: `RegisterCSignatures`
  goes through `CreateFunctionDeclaration`, so a differing llvm type is rejected on every route
  and order. As of the 2026-09-05 change the message also names the `.c`/header the prototype
  came from.
- The conflict message prints BOTH signatures in CFlat spelling, and the file-I/O tail is its own
  format string, emitted only for the stdio names `os.windows` republishes.

Coverage: `Test/errors/err_declarations.cb` (hand-written repeats, incl. the namespaced-core and
file-I/O arms), `Test/errors/err_extern_collides_with_core.cb` (the C-import route),
`Test/test_c_interop.cb` Section B (agreeing prototypes stay a silent no-op).
