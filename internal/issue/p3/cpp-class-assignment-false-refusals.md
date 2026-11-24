# Same-class C++ assignments clang accepts that cflat still refuses

**Summary.** Found while fixing CPPOPEQ (fix box 16, 2026-11-19). ASGACC (same box) fixed the by-value
call-result source (`a = ra.mkPub();` 109 - a register-returned class, any spelling: namespace / static /
member function, CFlat generic, field / `*p` / element destination, chained) and the prvalue destination
of a class with a user-declared copy / move operator= (`(ra.mkPub() = b).v` 107, counts match clang).
What is left is below; items 2, 4, 5 and 6 each need a maintainer ruling; 7 is an evaluation-order gap.

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

**4. Wording only:** an lvalue source of a class whose only usable assignment is the move
(`ra::MoveOnly`: private copy; `cppas::MoveOnly`: deleted copy) is refused (correct) with cflat's
`cannot copy-assign C++ class '...': it has no copy assignment operator cflat can call, ...` for EVERY
lvalue spelling - `a = b`, `a = (b)`, `a = *p`, `a = h.m`, `a = k > 0 ? b : c` - where clang says
`'operator=' is a private member of 'ra::MoveOnly'` (or its deleted text). Classifying an all-variable
`?:` join as an lvalue at RefuseUnassignableCxxSource (tried in ASGACC) flips
Test/errors/err_cpp_move_only_lvalue_assign.cb:33 (`t = c > 0 ? a : b;`, pinned to cflat's text) and
would split the join from the plain `t = a` leg (line 24, same pin). Taking clang's text means
rewriting those legs for every lvalue spelling together.

**5. Kept accepted, needs a ruling (wrongly accepted vs clang):** a class whose assignment clang
implicitly deletes because of a reference member (`struct BraceRefInner { int& r; int v; double d; };`
in Test/library/cpp_interop_ref_member.h) still assigns in cflat, rebinding `r`; clang says `object of
type 'cppref::BraceRefInner' cannot be assigned because its copy assignment operator is implicitly
deleted`. CPPOPEQ exempts that deletion cause (CClangExtract.cpp ResolveSameClassAssignment) because the
REFESC legs in Test/errors/err_cpp_ref_return_escape.cb (`result.inner = replacement;`, `if (c) result =
stable;`, loop overwrites, r4 forms) assign such aggregates to test reference-escape tracking. Refusing
like clang means rewriting those legs; a class with a user-declared copy constructor and a reference member
was already refused before CPPOPEQ.

**6. Prvalue destinations kept on the old path (pinned):** clang accepts `(ra::mkTriv() = b).v`
(`struct Triv { int v = 1; }`, implicit copy assignment) and `box[0] = box[0]` on `cpppa::ImplicitBox`;
cflat refuses both with `Left side of assignment is not an addressable lvalue.`, pinned by
Test/errors/err_cpp_proxy_assign_refused.cb (first leg). A class with no user-declared copy / move operator= (`cpppa::ProxyInt`, only `operator=(int)`) also keeps
the old prvalue path through that same gate: `proxyInts[0] = proxyInts[1] = 3;` writes 3 to both slots
(Test/test_cpp_interop.cb, leg 3531), while clang copy-assigns the proxy temporary from the inner result
(slot 0 stays 0, clang twin returns 3). A ref-qualified operator= on a prvalue is refused with cflat's
text for both qualifiers: `&` (clang: `no viable overloaded '='`) and `&&` (`(mkRQ2() = b).v`, clang
accepts, 104).

**7. Evaluation order (pre-existing):** `=` evaluates its left operand before its right (C++17: right
first). `rv.refI(1) = rv.sideI(2)` gives 12 on master and ASGACC, clang 21; the new prvalue-destination
path inherits it (`(mkO(1) = mkO(2))` ASGACC 12, clang 21).

**Fix direction.** 2: lower `[cpp]` struct assignment member-wise (EmitLoweredMemberwiseAssign) instead
of the generated record's operator=. 4 and 6: rulings on the pinned legs first; the ASGACC prvalue
reroute (MainListener_Assignment.cpp, `mk() = b` -> ordinary class path) is where 6 would widen.
