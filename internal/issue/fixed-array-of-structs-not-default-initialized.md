# A fixed array of structs is not default-initialized

Found 2026-07-16 while fixing the closure-capture double-free (surfaced as an ASAN fault in
`Test/test_function_ptr.cb`). Pre-existing, unrelated to `unique`. **Rule pinned 2026-07-16.**

**This contradicts a load-bearing claim**: `internal/plan/field-ownership-unique.md` recorded that
"every construction path zero-initializes - `new T()`, `new T[n]`, stack locals, and user-ctor entry",
and `unique`'s "trust the slot" decision rests on it. Those four paths were tested and DO hold.
**Array-of-struct elements were never tested, and do not.** The plan is corrected.

## The rule (pinned - do not re-derive it from a single test)

| declaration | zero-initialized? |
|---|---|
| scalar struct local (`T one;`) | **YES**, reliably - even with a deliberately dirtied stack |
| fixed array of structs (`T arr[N];`) | **NO**, never |

**The element type is NOT a factor.** An earlier investigation produced a table suggesting the
behaviour varied by element type (hand-written dtor vs synthesized dtor vs POD). **That table was
noise** - it was reading whatever stack garbage happened to be there, and the results inverted
between runs. `scratch/pin_arr.cb` and `scratch/pin_arr2.cb` (which dirties the stack with
`0x5A5A5A5A` first) show the same element type coming back clean in one run and garbage in the next.

Do not chase a per-type rule. There isn't one. Arrays are simply never initialized.

## Repro (`scratch/pin_arr2.cb`)

```cflat
struct A_plain { int n = 0; };
void dirty() { i64[64] junk; int i = 0; while (i < 64) { junk[i] = 0x5A5A5A5A5A5A5A5A; i++; } }
void probe()
{
    A_plain[2] a;   printf("array : n=%d\n", a[0].n);   // garbage
    A_plain one;    printf("scalar: n=%d\n", one.n);    // 0, always
}
extern int main() { dirty(); probe(); return 0; }
```

Note the field initializer (`int n = 0;`) is present and ignored for the array case.

## The dangerous case

`D_string[4]` yields a `string` with a garbage `_ptr` AND a garbage `_len`. The string destructor
decides whether to free by reading the **owned bit (high bit of `_len`)**. In observed runs the
garbage `_len` happened to have its high bit clear, so the destructor skipped the free. **With
different stack garbage it frees `_ptr`, which is also garbage.** A latent arbitrary-free, not a
benign read.

`Test/test_function_ptr.cb:1182` (`DispatchEntry[4] table;`) faults under `--asan` for this reason.
`test.bat` does not run `--asan`, so that file currently has NO asan coverage and the fault goes
unnoticed. The declaration is missing `= default`, which is the documented convention (CLAUDE.md:
"Always assign `default` to fields") - but the compiler should not rely on the convention to avoid
reading uninitialized memory, and the scalar path does not.

## Interaction with `unique`

A struct with a `unique` field, held in a fixed array, would have its garbage pointer freed by the
synthesized destructor at scope exit. **No `unique` failure is CONFIRMED** - `Box[4]` came back zeroed
in testing - but per the pinned rule above that is luck, not safety. Do not treat it as clean.

Note `unique T* x[N]` (a `unique` field that IS a fixed array) is separately REJECTED at parse time,
so this is only about arrays whose ELEMENT type contains a `unique` field.

## Fix direction

Make the fixed-array path run the same default-initialization the scalar path does. Find where a
scalar struct local gets its `zeroinitializer` (the synthesized default ctor path) and why the array
declaration path does not reach it.

**One judgement call to settle before implementing**: a large array (`T[1000000] big;`) would now be
memset. cflat is already committed to the zero-init model for scalars, so consistency argues for
doing it - but check `performance/` first, and consider whether an opt-out is wanted. This is the only
part of the fix that is arguably a design decision rather than a bug fix.

Verify with `--asan` - `test.bat` does not use it, so a regression here is invisible to the default
gate. `Test/test_function_ptr.cb` should stop faulting under `--asan` once this lands (its
`DispatchEntry[4] table;` is the canary).

## Related

- `internal/plan/field-ownership-unique.md` - records the "every construction path zero-initializes"
  claim, now corrected to name the four paths actually verified.
