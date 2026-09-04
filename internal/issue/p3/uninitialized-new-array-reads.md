# No detection for reads of uninitialized `new[]` memory

Filed 2026-08-29, after an intermittent `test_hpc_kernels` failure.

## Summary

`new T[n]` hands back uninitialized memory. Nothing in the toolchain detects a
read of it: `--asan` covers out-of-bounds and use-after-free, and
`--sanitize=ownership` covers use-after-move, but neither is MemorySanitizer.

The result is a heisenbug. Uninitialized heap usually arrives as fresh zeroed
pages, so the code silently does the right thing; when the allocator recycles a
dirty page it does not, and the failure looks like a numeric-library bug.

## Repro

`Test/test_hpc_kernels.cb` passed the Levenberg-Marquardt solver's INITIAL
GUESS as `double[] p = new double[2];` without ever writing it. `least_squares`
reads `p` on its first line, so the starting point was whatever the heap held.

    FAIL least squares p0   got 54186114339628824990109719867355742124409554043270097902660885083944006223344189461677543582134942796981461982789733415517523002402132073624588638111757318757408505135107179293955315049814186165839893276396212096789882743710982078464.000000 want 3.000000
    FAIL least squares p1   got -nan want -2.000000

The reproduction is a **recompile-and-run cycle, not a re-run**: 25 consecutive
runs of a single binary all passed, while fresh compile+run cycles failed 2 of
8. Anything that reproduces by re-running one exe will call this test stable.

The test itself is fixed (both initial guesses are now set explicitly), so this
issue is about the missing detection, not about that test.

## Root cause

`new[]` has no initialization contract and no instrumentation behind it. A
buffer read before it is written is indistinguishable from one written first.

Verified against emitted IR:

- `double[4] a = default;` is a stack `alloca` and `= default` emits explicit
  zero stores, so fixed-size locals are safe.
- `double[] a = new double[n];` is heap, allocated through `__active_allocator`
  - cflat's own PLUGGABLE allocator, not raw malloc.

That second point explains the "usually fine" behaviour and narrows the fix:
blocks backed by a fresh page arrive zeroed, while blocks the allocator recycles
from its free list carry the previous tenant's bytes. The nondeterminism is
cflat's own recycling, not the OS.

## Prior art

**C++** has the same default and compensates with three separate layers:

| form | initialized |
|------|-------------|
| `new double[n]` | no - indeterminate, reading is UB (what cflat does today) |
| `new double[n]()` or `{}` | yes, zeroed |
| `std::make_unique<double[]>(n)` | yes, value-initialized |
| `make_unique_for_overwrite<double[]>(n)` (C++20) | no - an explicit opt-OUT |
| `std::vector<double> v(n)` | yes |

Note the direction of travel: the recommended APIs zero by default, and C++20
had to ADD `for_overwrite` so callers could ask to skip it by name. Separately,
the MSVC debug CRT has filled fresh allocations with `0xCD` and freed ones with
`0xDD` for decades - option 1 below, shipping in production for thirty years.
C++26's "erroneous behaviour" rule (P2795) points the same way, though it
covers automatic variables rather than heap `new[]`.

**Rust** makes the read unrepresentable instead of detectable. `vec![0.0; n]`
zeroes; `Vec::with_capacity(n)` allocates without initializing but leaves
`len == 0`, so no element is reachable until written - the type system, not a
sanitizer, prevents it. `MaybeUninit<T>` is the explicit escape hatch and
`assume_init()` is `unsafe`. Miri catches violations in unsafe code.

cflat matches C++'s default exactly but has none of C++'s three compensating
layers (debug fill, MSan, zeroing recommended API).

## Fix direction - needs a ruling on which

1. **Debug-only poison fill.** Fill recycled blocks with a signaling NaN /
   `0xCD` pattern in Debug. Converts a rare silent wrong answer into a loud
   deterministic one, and matches the existing policy of giving Debug the
   safety net Release cannot have (Debug already links an assertions-enabled
   LLVM for the same reason). Because allocation goes through the pluggable
   `__active_allocator`, this is a LIBRARY change in the allocator - or even
   just a debug allocator implementation - with NO codegen work, which makes it
   far cheaper than the other two. It kills the *flakiness* specifically, which
   is the part that wastes reviewer time.
2. **Zero-initialize `new[]`.** Deterministic everywhere, but pays a memset on
   every allocation including the hot paths in `hpc/`, and it silently blesses
   read-before-write rather than reporting it.
3. **An uninitialized-read sanitizer** (`--sanitize=uninit`). The complete
   answer and the natural fourth member of the sanitizer family, but far the
   largest piece of work: it needs shadow state threaded through codegen.
   This is MSan / Miri.
4. **A zeroing spelling at the allocation site**, keeping the raw form for hot
   paths. This is exactly the C++ `make_unique` vs `make_unique_for_overwrite`
   split, and cflat already owns the vocabulary: `default` means
   zero-initialize for fixed-size arrays, and the house rule is already
   "always assign `default` to fields". Puts the cost where the caller can see
   it, but does nothing for code that forgets - so it pairs well with 1 rather
   than replacing it.

Recommendation: 1, optionally with 4. Option 1 removes the nondeterminism -
the property that makes this class expensive - without taxing Release or hiding
the defect, and it is a library-level change behind `__active_allocator`.
Option 2 is the one to avoid: it taxes every allocation including `hpc/`, and
it silently blesses read-before-write instead of reporting it, which is the
opposite of what both C++ and Rust concluded.

## Direction (maintainer, 2026-09-02): safety first on the common path

Supersedes the recommendation above. `new T[n]` is to become `array<T>.init(n)` (part of the
"hidden raw-array count desugars to a core owning array type" follow-up), and `init(n)` gets a
CONTRACT: every element reads as `default` afterwards. Primitives are zero-filled; struct
elements already run their constructor; pointer/interface elements are already memset.

Cost control, Rust/calloc style: route the fill through an `alloc_zeroed`-style entry on the
allocator so a fresh page or freshly mapped arena block (already zero) pays nothing and only a
RECYCLED block pays a memset - the recycled block is the only source of the flake. A second
entry, `init_capacity(n)`, skips the fill for kernels that overwrite the
whole buffer; that is `MaybeUninit` / C++20 `make_unique_for_overwrite`, spelled by name since
cflat has no `unsafe`. Option 1 (Debug poison fill) then applies to that path only.

Prior art checked: Rust `Vec::with_capacity` + `extend` keeps `len` at the initialized prefix so
an uninitialized read is unrepresentable; `vec![0; n]` is `alloc_zeroed`; `MaybeUninit`,
`spare_capacity_mut`/`set_len`, `Box::new_uninit_slice`, `ndarray::Array::uninit` are the
explicit unsafe opt-outs used by BLAS/FFT bindings.

## Landed (2026-09-02): array<T>.init contract and the new T[n] desugar

- `array<T>.init(n)` now honors the contract: primitive elements are memset to zero after the
  `new T[n]`; struct / pointer / interface elements were already `default`. The opt-out is
  `init_capacity(n)` (ruled 2026-09-03; landed as `init_uninit`, renamed the same day).
- `new T[n]` whose destination is `array<T>` desugars to `array<T>.init(n)`: decl-init, direct
  assignment, and `return` from a function declared to return `array<T>`. Per-site `alignas`
  on that `new` is rejected (the array frees through the plain deallocator).
- Returning a fresh heap array through a bare `T*` / `T[]` return is an error (the count is
  lost at the boundary). `move T*` and `array<T>` returns stay legal; `alias T*` is exempt as
  the documented hand-managed spelling. Six sites moved: core `regex._newStamp` (move),
  `arena_channel.acquire_arena` (`new page_arena<T>` single), `function.__closure_env_alloc`
  (alias); tests `makeAlignedFlat` (move), `ifaceViewFresh` (array<IShape>), `GOuter.make`.

Still open (this issue stays):
- `T* p = new T[n]` into a raw pointer is unchanged: primitives stay uninitialized. About 800
  such sites (core 174, Test 526, example 119) remain; the migration to `array<T>` is the
  vec follow-up in [[raw-array-count-desugar-direction]].
- The zero-fill is an unconditional memset, not the `alloc_zeroed` allocator entry sketched
  above; that optimization is worth doing only once the recycled-block allocators are measured.
- Debug poison fill for `init_capacity` (option 1) not started.
