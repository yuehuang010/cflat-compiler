# Returning a `std.function` by value from a CFlat function segfaults

Bucket: **p2** (silent crash, no diagnostic; blocks the natural "wrap the C++ callable in a CFlat
helper" idiom).

## Summary

A CFlat function whose RETURN type is a C++ class specialization - measured with
`std.function<int(int)>` - compiles clean and segfaults (exit 139) when the returned value is
called. The same value is correct while it stays a local, and correct when it crosses into or out
of a C++ function, so the defect is in the CFlat-function return of a nontrivial C++ class, not in
`std.function` itself.

Found during the review of the std::function member/return work (reviewer finding N5). It is
PRE-EXISTING: measured identical on master and on the branch. It is the only reason a lifetime
probe in that review had to be rewritten.

## Repro

`scratch/n5min.cb`:

```cflat
import cpp "functional";
int lt_twice(int x) { return x * 2; }
std.function<int(int)> lt_make()
{
    std.function<int(int)> f = std.function<int(int)>(lt_twice);
    return f;
}
extern int main()
{
    std.function<int(int)> g = lt_make();
    if (g(2) != 4) return 5;
    return 0;
}
```

Measured (macOS arm64, Release), both binaries:

```
compile=0
run=139        # SIGSEGV
```

- master build (`scratch/prefixbin/cflat`, commit 58df69e6): compile 0, run 139.
- branch build (fix/cpp-std-function-member-return): compile 0, run 139.

`scratch/rv/lt_c4.cb` is the longer form (a C++ call first, then the CFlat-returned callable) and
behaves the same.

Working neighbours, same session, same binaries:

- the callable stays a local and is called: correct.
- `cppfn.make_adder(7)` (a C++ function RETURNING `std::function`) stored in a CFlat local and
  called twice: correct (section M100 legs 1915-1918).
- a CFlat frame builds the callable, hands it to a C++ member that stores it, frame exits, C++
  fires it: correct (leg 1919).

## Root cause

Not investigated. The suspicion is that a CFlat function returning a nontrivial C++ class does not
construct into the caller's sret slot / does not copy- or move-construct the returned local, so the
caller reads a destroyed or never-initialized object. Confirm from the `--out-lli` of `lt_make`
before fixing.

## Fix direction

Make a CFlat function's return of a C++ class follow the same construct-into-slot and
copy/move-construct path a C++ function's by-value return already uses (M4b / M5), or refuse the
declaration with a diagnostic until it does. A silent crash is the worst of the three outcomes.
