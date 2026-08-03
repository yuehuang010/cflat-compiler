# A `simd<T,N>*` pointer declaration aborts the compiler

Filed 2026-08-02 while fixing [[empty-brace-initializer-never-seeds-and-crashes-on-defaults]].
PRE-EXISTING and unrelated to that fix: measured identical on `5a6580c` and on the `fix/emptybrace`
commit. Found because the empty-brace ruling's diagnostic suggests `nullptr` as the unambiguous
remedy, and for this one type the suggested remedy crashes.

Severity: compiler abort (rc 138, zero output), no diagnostic.

## Repro - measured with `-o` on Release builds of `5a6580c` AND `fix/emptybrace`

BOTH initializer spellings abort, not just `nullptr`:

```cflat
extern int main(){ simd<float,4>* sp = nullptr; printf("sp=%lld\n", (i64)sp); return 0; }
extern int main(){ simd<float,4>* sp = default; printf("sp=%lld\n", (i64)sp); return 0; }
```

```
compile rc = 138, no output, no diagnostic     // both spellings, both binaries
```

The `= default` face was found by mutation-testing the empty-brace reject leg, AFTER the first
draft of this file claimed only `nullptr` aborted. That claim was written from one measurement and
was incomplete - recorded here rather than silently corrected, since the difference matters: it
means BOTH remedies the empty-brace diagnostic names are broken for this one type.

The bare declaration is a clean (if oddly-worded) diagnostic on both binaries, so the abort is
specific to the initialized form:

```cflat
simd<float,4>* sp;
// simd<T,N> supports the static calls '.load(array, index)' and '.store(vector, array, index)'.
```

The empty-brace spelling `simd<float,4>* sp = {};` USED to reach the same abort by a different
route - the seeding arm stored a whole VECTOR into the 8-byte pointer slot. `fix/emptybrace`
rejects that spelling outright (`Test/errors/err_ptr_brace_init.cb` pins it), so `= {}` is now a
located diagnostic. Neither `= nullptr` nor `= default` is, and that is what is left open here:
`simd<T,N>*` currently has NO initialized declaration spelling that compiles.

## Root cause - NOT diagnosed

Not investigated. `ParseDeclarationSpecifiers` sets `declType.Pointer` from `declSpec->pointer()`
on the simd branch (`MainListener.h` ~3816), so a `simd*` type flows on with both `IsSimd` and
`Pointer` set, and something downstream almost certainly asks `GetType` for the vector rather than
the pointer. That is a citation, not a measurement - read the abort's stack before trusting it.

## Fix direction

Decide first whether `simd<T,N>*` is a supported type at all. If it is not, the bare-declaration
message above is the right diagnostic and it simply needs to fire for the initialized forms too.
If it is, the pointer-ness has to win over `IsSimd` wherever the slot type is computed. Per
CLAUDE.md, an LLVM/compiler abort gets a proper error message either way.
