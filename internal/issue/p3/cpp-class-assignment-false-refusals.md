# Same-class C++ assignments clang accepts that cflat still refuses

**Summary.** Found while fixing CPPOPEQ (fix box 16, 2026-11-19): the accept side of same-class
assignment on imported C++ classes still has refusals clang does not have. All measured identical on the
master exe (f21f9f7de) and the CPPOPEQ branch, cold and warm `-o`, so they pre-date that fix.

**Fixture** (`opeq.hpp`, used by every repro below):

```cpp
namespace ra {
struct Pub { int v = 1; Pub() {} Pub(const Pub&) = default;
  Pub& operator=(const Pub& o) { v = o.v + 100; return *this; } };
struct Prot { int v = 1; Prot() {} Prot(const Prot&) = default;
  protected: Prot& operator=(const Prot& o) { v = o.v + 100; return *this; } };
struct MoveOnly { int v = 1; MoveOnly() {} MoveOnly(MoveOnly&& o) : v(o.v) {}
  MoveOnly& operator=(MoveOnly&& o) { v = o.v + 50; return *this; }
  private: MoveOnly& operator=(const MoveOnly&); };
inline Pub mkPub() { Pub t; t.v = 9; return t; }
}
```

**1. A C++ by-value call result of a nontrivial class** (clang `a = ra::mkPub();` returns 109):

```cflat
import cpp "opeq.hpp";
extern int main() { ra.Pub a = ra.Pub(); a = ra.mkPub(); return a.v; }
```
cflat: `cannot assign to C++ class 'ra.Pub' from this expression; the source must be a 'ra.Pub' variable,
optionally written 'move <variable>'`. Same for a class with a public move assignment (clang 59). A class
whose only usable member is the move (MoveOnly) is accepted (59), as is a CFlat function returning `ra.Pub`.

**2. `[cpp]` struct assignment of any shape**, so the "protected base operator= is accessible from the
derived class" cell cannot be exercised (clang accepts all three: `h = g` 3, DP / DR `= std::move` 105 / 104):

```cflat
import cpp "opeq.hpp";
[cpp] struct HQ { int z = 0; };
[cpp] struct DP : ra.Pub { int z = 0; };
[cpp] struct DR : ra.Prot { int z = 0; };
extern int main() { HQ h = HQ(); HQ g = HQ(); g.z = 3; h = g; return h.z; }
```
cflat: `h = g` -> `clang: overload resolution selected deleted operator '='`; `h = move g` (and the DP / DR
moves) -> `'g' is not a 'HQ' value that can be assigned to a 'HQ'`.

**3. A prvalue destination** (clang `(ra::mkPub() = b).v` returns 107):

```cflat
import cpp "opeq.hpp";
extern int main() { ra.Pub b = ra.Pub(); b.v = 7; return (ra.mkPub() = b).v; }
```
cflat: `Left side of assignment is not an addressable lvalue.` (correctly refused shapes - deleted or
protected operator= - are refused with this text or clang's).

**4. Wording only:** `a = k > 0 ? b : c;` of lvalues of MoveOnly is refused (correct) with cflat's
`cannot copy-assign C++ class 'ra.MoveOnly': it has no copy assignment operator cflat can call, ...`
instead of clang's `'operator=' is a private member of 'ra::MoveOnly'`: the join's value category is not
proven at the CPPOPEQ guard, so it defers to the later path.

**5. Kept accepted, needs a ruling (wrongly accepted vs clang):** a class whose assignment clang
implicitly deletes because of a reference member (`struct BraceRefInner { int& r; int v; double d; };`
in Test/library/cpp_interop_ref_member.h) still assigns in cflat, rebinding `r`; clang says `object of
type 'cppref::BraceRefInner' cannot be assigned because its copy assignment operator is implicitly
deleted`. CPPOPEQ exempts that deletion cause (CClangExtract.cpp ResolveSameClassAssignment) because the
REFESC legs in Test/errors/err_cpp_ref_return_escape.cb (`result.inner = replacement;`, `if (c) result =
stable;`, loop overwrites, r4 forms) assign such aggregates to test reference-escape tracking. Refusing
like clang means rewriting those legs; a class with a user-declared copy constructor and a reference member
was already refused before CPPOPEQ.

**Fix direction.** 1: route a same-class C++ prvalue call result through the same temp + operator= pick a
CFlat call result takes. 2: lower `[cpp]` struct assignment member-wise (EmitLoweredMemberwiseAssign) instead
of the generated record's operator=. 3: lower `prvalue = x` on a class as the member call on the temp.
4: classify an all-lvalue `?:` join as an lvalue at RefuseUnassignableCxxSource.
