Suggested bucket: p3 (capability gap, clean refusal, no wrong values).

# A file-scope `const` initialized from an ENUMERATOR is not accepted as a compile-time constant

Found 2026-09-17 while fixing the scoped-enum non-type-template-argument issue (probe corpus
`scratch/ent_c09*.cb` in the `fix/cpp-enum-nttp` worktree). Not part of that fix: different root
cause (global-initializer constant folding), and it is not C++-interop specific.

## Summary

An enumerator is a compile-time constant everywhere else, but a file-scope variable initialized
from one is refused. The integer spelling of the same value is accepted, so the gap is the
enumerator leaf in the global-initializer folder, not the `const` itself.

## Repro

```cflat
enum Local : int { A = 1, B = 2 };
const Local lk = Local.B;               // ent_c09j.cb(2,17): global variable initializer must be
extern int main() { return (int)lk; }   // a compile-time constant
```

Measured on master 6e48e5b4 (macOS arm64, Release), compile exit 1 each time:

```
const Local lk = Local.B;               -> global variable initializer must be a compile-time constant
const cppi.Color k = cppi.Color.Green;  -> same (C++-bound SCOPED enum)
const cppi.Plain p = cppi.P_TWO;        -> same (C++-bound UNSCOPED enum)
const int ki = 3;                       -> accepted (control; usable as a C++ NTTP, exit 3)
```

So all three enum flavours - CFlat, C++ unscoped, C++ scoped - fail identically, and the plain
integer control works. A LOCAL `const` of either flavour is a separate, also-refused shape
(`'k' has no C++ spelling, so it cannot be a template argument ...` when fed to a C++ template);
that one is about named locals, not about folding.

## Fix direction

Teach the global-initializer constant folder the `<enum>.<member>` leaf - the value is already
available from `TryGetEnumMemberInt`, which `FoldCompileTimeIntLeaf` (MainListener.h) uses for the
scan-time path. Accept set to freeze first: `const int`, `const` from a literal expression, and
every existing global-initializer refusal.
