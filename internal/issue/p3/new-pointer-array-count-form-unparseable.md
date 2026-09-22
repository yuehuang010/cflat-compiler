# `new int*[2]` does not parse

Bucket: grammar; needs a look at CFlat.g4 (no parser predicates allowed). Filed 2026-09-04
during the q10 merge verification, master 073f5948.

## Summary

`int*[] pv = new int*[2];` fails with `unexpected '[' here; expected {... 'new' ...}` at the
`[`: the `new T[n]` form does not accept a pointer element type. The workaround is a fixed
array `int*[2] cells; int*[] pv = cells;`. `new int[2]` and `new Foo*` parse. Note the raw
heap-array area is ON HOLD (2026-09-04 ruling); this is only the parse of the spelling, but
check with the maintainer before touching `new` grammar.

## Sibling measured 2026-09-21: `new int*(a)` parses as a MULTIPLY and dies in the LLVM verifier

```cflat
extern int main() { int* a = new int; int** pp = new int*(a); delete a; return 0; }
```

`Error: module verification failed.` - `Integer arithmetic operators only work with integral
types! %18 = mul ptr %16, %17`. The text is read as `(new int) * (a)`, and the pointer-times-
pointer multiply reaches codegen with no diagnostic. Identical on master 96bbe1a8 (pre-existing,
found while verifying fix/new-primitive-init, where `new T(v)` gained the C++ meaning for
non-class T - the pointer-element spelling is the one form that fix cannot reach). Two parts:
(1) same grammar gap as above - `new` does not take a pointer element type before `(`/`[`;
(2) DONE 2026-09-21: independent of the grammar, unsupported pointer binary arithmetic and
bitwise operators now produce a LogError naming the operator and operand types, never a verifier
failure. Pointer +/- integer, pointer - pointer, and comparisons remain legal. Part (1) stays on
hold.
