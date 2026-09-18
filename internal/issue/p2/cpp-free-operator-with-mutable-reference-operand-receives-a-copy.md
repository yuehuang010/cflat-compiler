Bucket: p2 (silent wrong value: the mutation lands on a copy, no diagnostic)

# A C++ free operator whose operand is a non-const reference mutates a COPY of the CFlat object

Found 2026-09-17 in review of fix/cpp-string-freeops (macOS arm64, Release). Pre-existing: the
NON-template free-operator path has the same defect on master 884a0c08; the operator-TEMPLATE
path newly reaches it since that fix (before it the call was refused outright).

## Summary

For a free operator declared `Sink& operator|(Sink& s, const Tag& t)` (stream-style, the
operand is mutated and returned by reference), the CFlat call site `s | t` loads `s` into a
fresh alloca and passes THAT to the wrapper. The mutation lands on the copy, the real `s` is
unchanged, and for the template form the `T&` return collapses to a by-value copy under the
wrapper's `auto`. C++ semantics: `s.n_` must be 5 after `s | t`.

## Repro

Header (`scratch/rev/rev_h.h` in the review worktree; self-contained excerpt):
```cpp
namespace revns {
class Con { public: Con(int v) : v_(v) {} int v_; };
class Sink3 { public: Sink3() : n_(0) {} int n_; };
class Tag { public: Tag() : t_(5) {} int t_; };
inline Sink3& operator|(Sink3& s, const Tag& t) { s.n_ += t.t_; return s; }   // non-template
class Sink2 { public: Sink2() : n_(0) {} int n_; };
template <class T> Sink2& operator|(Sink2& s, const Con& c) { s.n_ += c.v_; return s; }  // template
}
```

```cflat
import cpp "rev_h.h";
extern int main()
{
    revns.Sink3 s = default;
    revns.Tag t = default;
    revns.Sink3* r = s | t;
    return s.n_ * 10 + r->n_;   // C++: 55. Measured: 5 (s.n_ == 0, r points at the copy)
}
```

Template form (`revns.Sink2 r = s | c;` with `Con<int>(5)`): master refuses
(`no overload of 'operator|' matches the given arguments`); fix/cpp-string-freeops compiles and
returns 5 with `s.n_ == 0`. IR at the call site: `%2 = alloca %revns.Sink2; store ...;
call @__cflat_tpl_...(ptr %2, ...)`.

## Root cause

The by-reference operand of a free operator is materialized like a by-value argument
(`LLVMBackend_CInterop.cpp` ~6316 `infixForm` wrapper emission and
`MainListener_Expressions.cpp` ~8934 `callOperatorTemplate`, plus the non-template free-operator
call path): the wrapper's parameter is spelled by value / `auto`, so C++ re-resolves `p0 @ p1`
against a copy. A guard keyed on `selected->parameterTypes` would be unsound because
`selected` only picks the group; the wrapper's own parameter spellings decide what C++ sees.

## Fix direction

Spell the wrapper parameter as the operator's declared reference kind (`T&` stays `T&`, pass the
object's address, no temporary) and return `T&` as a pointer at the CFlat boundary, for BOTH the
template and non-template free-operator paths - same family as member operators, which already
receive `this` by address. Accept set: const-reference and by-value operands must keep their
current lowering; a CFlat rvalue (call result) bound to a non-const `T&` operand must be refused,
matching internal/issue/p3/cpp-rvalue-binds-non-const-scalar-reference-member-parameter.md.
Legs: the two repros above asserting 55, plus chaining `s | t | t` asserting 10 on the real
object.
