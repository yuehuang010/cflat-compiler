# Passing a nontrivial C++ class BY VALUE to a CFLAT function is a bitwise copy - no copy ctor, no parameter destructor

Found 2026-09-16 by the C++-interop bug bash round 3 (macOS arm64, Release, worktree at master 2798eb1a).
Parameter-side twin of `internal/issue/p2/cpp-return-class-by-value-uses-synth-copy.md` (that one is
the RETURN side of a CFlat function; this is the argument side, and it corrupts the heap).

## Summary

NOT STARTED: same family as internal/issue/p2/cpp-class-copy-from-field-skips-copy-ctor.md (CFlat-side copy of a C++ class), which is HELD by maintainer ruling for a later CFlat-interop plan; needs the same ruling.

A CFlat function declared `int take(bb3l.Trk t)` over a nontrivial foreign C++ class receives the
argument as a raw `load` + register-passed struct value:

* the C++ COPY constructor never runs, so a class that owns a resource ends up with two owners of
  the same bytes, and
* the parameter is never destroyed in the callee, so the counts are silently "balanced" for a
  trivial class and silently WRONG for anything with an invariant.

With `std.string` this is an immediate use-after-free: the callee's bitwise duplicate reallocates
the shared buffer, the caller's string is left pointing at freed memory, and the program aborts.

C++ callees are correct (Test section M5 covers them) - only a CFLAT callee is affected.

## Repro 1 - counts (compile 0, run 0)

`scratch/bb3_life.h`: `struct Trk { int v; Trk(int x); Trk(const Trk&) { copies()++; } ~Trk() { dtors()++; } ... }`

```cflat
import cpp "bb3_life.h";
extern int printf(const char* f, ...);
int take(bb3l.Trk t) { t.v = t.v + 1; return t.v; }
extern int main()
{
    bb3l.reset();
    { bb3l.Trk a = bb3l.Trk(10);
      int r = take(a);
      printf("take=%d ctor=%d copy=%d move=%d dtor=%d\n", r,
             bb3l.n_ctor(), bb3l.n_copy(), bb3l.n_move(), bb3l.n_dtor()); }
    printf("end ctor=%d copy=%d move=%d dtor=%d\n",
           bb3l.n_ctor(), bb3l.n_copy(), bb3l.n_move(), bb3l.n_dtor());
    return 0;
}
```

Measured:

```
take=11 a.v=10 ctor=1 copy=0 move=0 dtor=0     <-- C++ requires copy=1
end ctor=1 copy=0 move=0 dtor=1                <-- C++ requires dtor=2 (param + local)
```

## Repro 2 - heap corruption with std.string (compile 0, run 133 = SIGTRAP from malloc)

```cflat
import cpp "string";
extern int printf(const char* f, ...);
int grow(std.string s)
{
    s.append("-XXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXX");
    return (int)s.size();
}
extern int main()
{
    std.string a = std.string("0123456789012345678901234567890123456789");
    printf("before size=%d str=%s\n", (int)a.size(), a.c_str());
    int r = grow(a);
    printf("callee size=%d\n", r);
    printf("after  size=%d str=%s\n", (int)a.size(), a.c_str());
    return 0;
}
```

Measured (twice, fresh compiles):

```
before size=40 str=0123456789012345678901234567890123456789
callee size=89
after  size=40 str=<garbage byte>
Trace/BPT trap: 5          (libmalloc abort - the caller's buffer was freed by the callee's realloc)
```

The caller's 40-byte string is out of SSO range, so both copies share one heap buffer; `append`
reallocates and frees it under the caller.

## Root cause

`x64/Release/cflat scratch/bb3_cfval.cb -i scratch --symbol-dump-ir function:main`:

```llvm
  %a = alloca %bb3l.Trk, align 8
  %0 = call ptr @_ZN4bb3l3TrkC1Ei(ptr %a, i32 10)
  %1 = load %bb3l.Trk, ptr %a, align 4                    ; bitwise load
  %2 = call i32 @"_take$int$.1$bb3l.Trk"(%bb3l.Trk %1)    ; passed by value, no copy ctor
```

The CFlat-to-CFlat call path treats a foreign C++ class parameter as a plain CFlat struct: load and
pass in registers. Nothing copy-constructs into the parameter slot, and the callee's epilogue has no
destructor for it. This is the same missing arm as the return-side issue: `TryDeclareForeignCxxLocal`
/ `EmitCxxCopyOrMoveConstruct` exist for declarations but the CFlat call-argument and
parameter-prologue paths do not use them.

## Fix direction

For a CFlat function parameter whose type is a nontrivial foreign C++ class, follow the Itanium rule
the C++-callee path already implements: pass indirectly (caller-allocated temporary), copy- or
move-CONSTRUCT the argument into that temporary with the class's own constructor, and destroy the
parameter object in the callee (or in the caller, per the ABI arm already chosen for C++ callees -
whichever the existing C++ call path uses, so the two stay consistent). If full support is out of
scope for now, reject a nontrivial C++ class parameter on a CFlat function with a diagnostic
pointing at pointer parameters, the way the deleted-copy-ctor case already does at a C++ call.

Acceptance: repro 1 prints `copy=1` in scope and `dtor=2` at the end; repro 2 prints the original
string after the call and exits 0.

Suggested bucket: p1 (clean compile, use-after-free and heap abort on ordinary code).
