# Forwarded closures and function-pointer references: refusals left after T525

T525 (2026-10-26) fixed every wrong-code cell this file listed. `__cflat_closure<>` now has the CFlat
closure pair layout `{code, env}` (ownership is the env's tag bit), so a closure VARIABLE crosses into
a generated C++ wrapper as its own slot, spelled `*static_cast<__cflat_closure<SIG> *>(pN)`. `F &` /
`F &&` bind the caller's object exactly as clang binds the caller's lambda. Fixed: forwarded results,
std::function(std::ref(f)), BorrowDtor<F>, Mixed<F>, unique_ptr<F, NoOp>, optional<Mixed2<F, G>>, and
a later `f = ...` seen through a kept reference. Also fixed: Decor<int *, int (*)(int)> registration
(a `(*)` inside template arguments was parsed as the result's declarator) and a nullptr `?:` arm
next to a thin function pointer. Legs: T525 block in `Test/test_cpp_interop.cb` (52501-52517),
`Test/errors/err_cpp_forwarded_closure_dangles.cb`.

No wrong-code cell remains. What is left is located refusals of programs clang accepts:

## 1. A moved or temporary closure into a result that keeps a reference

```cflat
std.cflat_t28.call5(std.cflat_t28.fwd(move f));   // clang 20 (std::move(f) leaves f alive)
t28.holdptr(move f);                                // clang keeps &f
```

Refused with "caller-owned C++ closure slot" (T419 layout facts). CFlat `move f` drains `f` into a
temporary that dies at the end of the statement, so the kept reference would dangle. Matching clang
would need a caller-frame temporary destroyed at the end of the full expression plus a rule for what
`move` means when the source stays alive. A lambda literal argument has no typed context and is
refused earlier (return type inference), so it never reaches this check.

## 2. `move` of a closure variable lent by reference to C++

```cflat
auto h = t28.holdfwd(f);
Lambda<int(int)> g = move f;   // refused: h may call f, and `move` leaves f null
```

Refused only on evidence that the C++ result keeps a reference to `f` (`NamedVariable::CxxClosureLentTo`,
set by `MarkKeptCxxClosureArguments` on both the template and free-function paths). The evidence is either
(a) the T419 layout facts of a by-value class result (`R:`/`P:` indirections, the visible-field rule), or
(b) the harvested construction-site fact `closureKeptParams` (cache key `pckeep`, v293): in the selected
wrapper body, the closure parameter reaches std::ref/cref/reference_wrapper, `&`, a reference member,
a reference variable, a by-reference lambda capture, or a reference return. A plain copy or move
(`vector<F>`, `holdfncopy`, std::function(f), Decor, shared_ptr) is accepted, matching master and clang.

Remaining false refusals, all accepted by clang:
- a user template holder that owns its F by value but that the layout rule cannot prove (`OwnedBox<F>`;
  `holdownedbox(f)` was refused on master too);
- moves after the holder is dead, or in a branch disjoint from the lend: the guard is flow-insensitive;
- `copy_return` (a by-value closure return) is a separate refusal from before this change.
A copy (`Lambda<int(int)> g = f;`) is the workaround. Anything the walk cannot see counts as kept (r3):
a callee with no visible body, a virtual dispatch, or a call through a function pointer that takes the
closure by reference or pointer; and every parameter still under analysis when the depth-32 limit is
hit. So an opaque keeper (`t525.holdopaque`, declared only) is also refused after `move`.
A callee that only returns the closure by reference (std::as_const, a user identity helper) is an
alias: the caller's use of its result is classified like a direct use (r4).

## 3. A result that is a reference to the closure itself

`template<class F> F&& pass(F&& f)`: `t525.call5(t525.pass(f))` prints 20 in clang. Refused: "returns a
reference to the CFlat closure it was given". A `__cflat_closure<> &` result has no CFlat value form.
It would need an alias closure result whose slot is the referenced object.

## 4. Free non-template function returning a function-pointer reference

`inline Fp& fwdfp(Fp& p)`: `t525.fwdfp(fp)(5)` gives 7 in clang. Still refused by the importer
("return type 'int (*&)(int)' is an unsupported reference to a function pointer"). A spike that only
dropped the refusal got the direct call right (7), but `function<int(int)> q = t525.fwdfp(fp);` copied
the slot address as the function pointer (exit 139). The call site needs to load through a returned
function-pointer reference the way it does for `T *&`.

Found by: t28 sibling audit (2026-10-03); T56, T69, T419, T525 rounds.
