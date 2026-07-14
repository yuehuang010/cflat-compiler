# Per-thread FP environment is a silent no-op on Linux

`core/thread.cb`'s `__fp_apply` (and `fp_disable_traps`) implement the per-thread
floating-point environment on two of three platforms:

- **Windows**: `_controlfp_s` -> MXCSR (traps + flush-to-zero). Works.
- **macOS/arm64**: `fegetenv`/`fesetenv` round-trip `fenv_t.__fpcr`; setting FPCR.FZ
  (bit 24) flushes denormal inputs and outputs to zero. Works (see `test_fpenv`).
- **Linux**: still `// no-op`.

So `t.start(fn, ctx, FP_FLUSH_DENORMALS)` on Linux silently does nothing - the thread
runs with the default FP environment and the caller gets no diagnostic. `test_fpenv` is
skipped on Linux in `test.sh` for exactly this reason (the skip is OS-gated, not blanket).

## Fix direction

Linux x86-64: the knob is MXCSR - flush-to-zero is bit 15 (FTZ) and denormals-are-zero is
bit 6 (DAZ). glibc's `fenv_t` carries `__mxcsr` as its last word, so the same
`fegetenv`/`fesetenv` shape used on Darwin works, just against a different (larger)
struct layout - it is NOT the 2 x u64 Darwin `fenv_t`, so it needs its own
`if const (__LINUX__)` struct rather than reusing `__FEnvArm64`.

Layout (glibc x86-64, `bits/fenv.h`): 7 x u16 + u32 + u16 x2 + u32 + u16 x2 + **u32 __mxcsr**
- i.e. `__mxcsr` sits at byte offset 28 in a 32-byte struct.

## Traps are a separate, harder question

`FP_TRAP_*` cannot be honoured on Apple Silicon at all: the FPCR trap-enable bits are
RAZ/WI, so ARM64 macOS hardware does not implement FP exception traps. The macOS
implementation therefore deliberately ignores the trap bits and applies only FTZ. Any
future "traps everywhere" story has to account for that - it is a hardware limit, not a
porting gap. Linux x86 *can* trap (unmask via MXCSR), so a Linux port could support the
full config while macOS supports only the FTZ subset.
