# Assigning into a `unique T*` local declared WITHOUT an initializer drops the rest of the function

Filed 2026-08-09 by `fix/arrslot` while probing the type axis of the fixed-array-element
destination, and re-rooted in review the same day: it is NOT array-specific and NOT scope
teardown. Unrelated to that fix's family (this is a POINTER destination, and the loss happens at
the assignment, not in an owning-struct store), so it was left out deliberately.

Severity: **abort** (`--no-opt`: rc 134 inside `free()`), which the default optimized build folds
into silently skipped statements. Read the optimized shape as the symptom, not the defect - see
the root cause below, which was corrected on a second measurement pass.

## Repro (minimal, no array, no block)

```cflat
struct Res { int id = 0; };
extern int main()
{
    unique Res* p;              // declared with NO initializer
    printf("a\n");
    p = new Res();              // everything below this is dropped
    printf("b\n");
    p->id = 1;
    printf("c %d\n", p->id);
    return 7;
}
```

-> compiles 0, prints only `a`. Exit code depends on the optimizer: **`--no-opt` aborts, rc 134**;
the default optimized build exits rc 7. Replacing `return 7` with `return <a value the dropped
statements compute>` returns 0 in the optimized build, i.e. those statements really do not run
there.

The array spelling this was originally filed as is the same defect:

```cflat
{ unique Res*[2] arr;
  arr[0] = new Res(); arr[0]->id = 1;
  printf("in %d\n", arr[0]->id); }   // this printf survives
printf("end\n");                     // NEVER RUNS
return 7;
```

## Controls (all correct on the same binary)

- `unique Res* p = new Res();` (initialized at the declaration) - runs every statement.
- `unique Res* p = nullptr;` then `p = new Res();` - runs every statement. **The initializer is
  the whole difference**, which is why the original filing's scalar control looked clean.
- a NON-unique `Res*[2]` local prints its trailing statement.

Measured identical on `7beb979` and on `fix/arrslot`.

## Root cause (MEASURED - this section was wrong on the first filing)

The first filing guessed "no `alloca`, the statement walk stops at the assignment". The `--no-opt`
IR refutes both halves: `%p = alloca ptr` IS emitted, `store ptr %0, ptr %p` IS emitted, and the
whole tail of the function (`printf` "b", the field store, `printf` "c", the scope-exit cleanup)
is emitted ahead of the `ret`. Nothing is truncated at compile time.

The defect is that the declaration allocates the slot but never **initializes** it. The
reassignment's drop-old then reads that garbage:

```llvm
  %p = alloca ptr, align 8                      ; no store follows the declaration
  ...
  %uq.ptr   = load ptr, ptr %p                  ; GARBAGE
  %uq.isnull = icmp eq ptr %uq.ptr, null        ; false
  %uq.same   = icmp eq ptr %uq.ptr, %0          ; false
  br i1 %uq.skip, label %uq.after, label %uq.delete
uq.delete:
  call void @Res.dtordeferred(ptr %uq.ptr)
  call void @"_operator delete_void_U8Ptr_"(ptr %uq.ptr)   ; free(garbage) -> SIGABRT
```

That `free()` is the abort. It also explains the optimized build's appearance: a load from an
uninitialized alloca is `undef`, so the optimizer is free to fold the branch and the code after
it, which is why the tail vanishes there and a constant `return 7` still survives. Both controls
are clean for one reason - `= nullptr` and `= new Res()` each WRITE the slot, so the drop-old
loads a real value. The file's original "the initializer is the whole difference" observation is
right; its explanation was not.

The `unique T*[N]` array spelling is the same defect (the array local's slots are not zeroed
either): `--no-opt` rc 134 on both binaries.

## Fix direction

Zero-initialize the slot at an uninitialized `unique T*` declaration, exactly as the `= nullptr`
spelling does - the drop-old sequence already handles null correctly, so that alone closes both
spellings. Do the same for `unique T*[N]` element storage. (Rejecting the declaration outright is
the alternative, but `unique T* p;` followed by a later assignment is an ordinary idiom and every
other local kind supports it.) Add regression legs to `Test/test_move.cb` next to the existing
unique-array-element legs: assign into an uninitialized `unique T*` local and into a
`unique T*[N]` element, then assert a trailing observable side effect - and run them under
`--no-opt` reasoning, i.e. assert a VALUE the trailing statements compute, not just an exit code.
