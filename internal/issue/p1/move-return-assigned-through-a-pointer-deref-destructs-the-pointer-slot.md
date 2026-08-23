# `*out = f();` with a `move`-returning `f` destructs the POINTER SLOT and skips the drop-old

Filed 2026-08-22 from an external report (cflat v0.11.0, quant-backtester project, issue 18).
Reproduced on `master` at the time of filing, macOS arm64 Release.

Severity: **heap corruption + hard abort**. `free()` is called on a stack address. Reported from
real code (`edgar_fetch.cb`, `loadOrFetch`) as an intermittent segfault, and under ASan as a
stack-buffer-overflow inside `string.dtor` reading past an 8-byte slot in the callee's frame.

## Repro

```cflat
import "string.cb";
move string makeString() { return "hello" + " world"; }
bool fill(string* out)
{
    *out = makeString();
    return out->length() > 0;
}
extern int main()
{
    string body;
    bool ok = fill(&body);
    printf("%d [%s]\n", ok, body.data());
    return 0;
}
```

Linked exe: aborts with exit code 134 before any output (stdout is lost with the abort).
`--run` (JIT) happens to print `1 [hello world]` and exit 0 - the corruption is not fatal there,
so **do not use `--run` to judge this fixed**. Build the exe.

## What is emitted

```llvm
define internal i1 @_fill_bool_stringPtr_(ptr %out) {
entry:
  %out1 = alloca ptr, align 8              ; <-- slot for the POINTER, 8 bytes
  store ptr %out, ptr %out1, align 8
  %0 = call %string @_makeString_string__()
  store %string %0, ptr %out, align 8      ; <-- no drop-old destruct of *out
  ...
  call void @string.dtor(ptr nonnull %out1) ; <-- dtor on the POINTER slot
  ret i1 %5
}
```

Two defects in one path:

1. **The pointer slot is registered as a destructible `string` local.** A slot named `out` of the
   *pointer* base type is spilled at entry and destructed at scope exit as if it were a `string`.
   `string.dtor` reads `{ptr,i32}` out of an 8-byte slot (OOB) and frees field 0, which holds
   `&body` - a stack address. That is the abort.
2. **No drop-old.** The store into `*out` overwrites the destination without destructing its old
   value. Harmless in the repro (`body` is an empty default string) but leaks whenever `*out`
   already owns a buffer.

## Trigger, narrowed

The discriminator is a **`move`-returning callee's result assigned directly through a deref of a
pointer PARAMETER**. Neighbouring spellings, all measured on the same build:

| Spelling | Emitted |
|---|---|
| `void f(string* out) { *out = makeString(); }` (`move string` return) | **BUG**: spurious `%out1` slot + `string.dtor(%out1)`, no drop-old |
| `void f(string* out) { out[0] = makeString(); }` (`move string` return) | **BUG**: identical |
| `string mk2() { string s = "hi"; return s; }` then `*out = mk2()` (**non-`move`** return) | correct: `string.dtor(%out)` drop-old, then a deep-copy store, no extra slot |
| `void f(string* out) { string s = makeString(); *out = s; }` (named local RHS) | correct: drop-old + copy + dtor of `s` |
| `void f(string* out) { *out = "abc"; }` (literal RHS) | correct: drop-old + store |
| `void f(Box* bx) { bx->s = makeString(); }` (struct FIELD through a pointer) | correct: drop-old + store |
| `string b; string* p = &b; *p = makeString();` (pointer LOCAL aliased to a known base) | no spurious dtor - the deref is folded back onto `b` |
| `void f(list<int>* out) { *out = mkl(); }` (`move list<int>` return) | **partial bug**: no spurious slot, but **no drop-old** - the old list leaks |

So symptom (2), the missing drop-old, is general to a `move`-return RHS stored through a deref of
a pointer parameter, for any owning value type. Symptom (1), the destruct of the pointer slot, is
specific to `string`, which carries extra owned-temp tracking that the other owning types do not.

## Fix direction

The deref-assign arm in `MainListener_Expressions.cpp` (the `destIsLocalOwningVar` /
`destIsStructField` drop-old block around line 3035, and the owning-deref transfer arm above it at
~2968) gates the drop-old on the destination being an `AllocaInst`/`GlobalVariable` or a struct
field. A **function argument** is neither, so a deref of a pointer parameter falls out of every
arm and reaches the plain store. Two things to settle:

- Admit a deref-of-parameter destination to the drop-old set. Careful: this is the same slot whose
  ownership meaning is caller-dependent - see the related open item on `string*` parameter slot
  semantics depending on argument provenance (`internal/issue/p2/string-pointer-param-slot-
  semantics-depend-on-argument-provenance.md`). A drop-old is correct when the slot is a LIVE
  owner (the fixed-array / by-address-of-a-local caller) and wrong when it is a raw uninitialized
  `new string[n]` element. The non-`move` return already drops old unconditionally here, so the
  `move` path is merely inconsistent with its own sibling - matching the sibling is the
  conservative move, not a new policy.
- Find and remove the registration that gives the parameter a destructible slot. The spilled
  alloca is created with the parameter's *pointer* base type but registered under the pointee's
  type name (`string`), so the scope-exit teardown emits `string.dtor` on it. Whatever creates
  that slot must either not register it, or register it with `TypeAndValue.Pointer` respected.

## Acceptance

- The repro above, built to an exe (not `--run`), prints `1 [hello world]` and exits 0.
- The emitted `fill` has no `alloca ptr` named after the parameter and no `string.dtor` on it.
- `*out = <move-returning call>` destructs the old `*out` before storing, for `string` and for a
  container type such as `list<int>`.
- Full suite green: `./test.sh Release` on macOS/Linux, `test.bat` on Windows. Add the repro as a
  case in an existing test file (do not create a new one); `Test/test_move.cb` or the string tests
  are the natural homes.
