# C++ by-value prvalue argument gets an extra move in template wrapper

Status (2026-11-10): the by-value prvalue beside a later call argument is elided into a FREE
function template's parameter when every argument already has its selected parameter's type
(gate: LLVMBackend_CxxRequestFunctions.cpp, exactSelectedParameters). Any conversion among the
later arguments (narrowing, derived-to-base, `?:` of a derived class) keeps the wrapper, so C++
converts it; that path is correct in value but still makes the one extra move master made.

Still one extra move (clang++ -std=c++20 makes none; probes in scratch/prvarg_tmp/shapes*.cb,
fixed build = master):

- C variadic template `cvv(T x, int n, ...)` with a later call: `p.cvv(p.Ev(1), 2, p.side())`
  (fixed m1 d2; clang m0 d1).
- Non-constant default `int z = side()`: `p.d2(p.Ev(1), p.side())` (fixed c1 m1 d2; clang c1 m0 d1).
  Without a later call argument the default case matches clang.
- Class default `Ev z = Ev(3)`: `p.d4(p.Ev(1), p.side())` (fixed c2 m1 d3; clang c2 m0 d2).
- String literal bound to `U&&`: `p.pa(p.Ev(1), "ab", p.side())` (fixed m1 d2; clang m0 d1).
- Trivial dtor with a non-trivial move (`Sm`): `p.f1(p.Sm(7), p.side())` (fixed m1 d0; clang m0 d0).
- Name containing `.operator`: `p.operator_tmpl(p.Ev(1), p.side())` (fixed m1 d2; clang m0 d1);
  the symbolNamed text gate at LLVMBackend_CxxRequestFunctions.cpp:3815 falls back safely.
- Later argument that converts (round 2 finding; value correct, see Status): `p.narrow_long(p.Ev(1), q)`
  with `long q` (clang m0 d1; fixed m1 d2).
- Derived-to-base by-value later argument: `p.ctb(p.Ev(1), e)` with `Dt : Tb` (fixed m1 d2;
  clang m0 d1).

Repro for the free template shape (clang m0 d1, fixed m0 d1 when the arguments are exact):

    struct Ev { int value; Ev(int n); Ev(const Ev&); Ev(Ev&&); ~Ev(); };
    template<class T> int free_tmpl(T x, int y) { return x.value * 1000 + y; }
    free_tmpl(Ev(7), next())   // clang: ctor 1 move 0 dtor 1

Still open:

1. Member function templates (`obj.tmpl(Cls(...), later_call())`, by-value `T`): one extra move
   and one extra dtor. Leg 9359 (Test/test_cpp_interop.cb:6633) pins move == 1; clang++ gives 0.
   Changing 9359 needs maintainer authorization (tests are read-only for the fix box).
2. A deleted or private move ctor at a prvalue argument of a function template: CFlat refuses
   with "call to deleted constructor" where clang++ -std=c++20 accepts (guaranteed elision).
   Master behaviour; the template wrapper's body must move the argument, so the refusal comes from
   wrapper generation, not from the call lowering.

Root cause: a call to a function TEMPLATE with a by-value class prvalue argument and a later call
argument (`f(C(7), next())`) cannot build the prvalue inside the wrapper (the later argument's
side effect must run after the construction, canDeferCxxConstruction). The caller materializes it
and passes its address to the `__cflat_tpl_` wrapper, whose body `f(static_cast<C&&>(p0), p1)`
move-constructs the by-value parameter. Non-template free functions and non-template members are
called on the C++ ABI directly and are elided.

Fix on this branch: RequestCxxFunctionTemplateOnce hands free-template calls to
SwitchCxxDirectByValueCall (the selected instantiation is called on its own by-value ABI, as for
free functions), and the call site resolves `__cflat_byval_` names
(MainListener_PostfixExpression.cpp, resolvedName condition). Leg: Test/test_cpp_interop_bridge.cb
PRVARG block (44621-44624) with Test/library/cpp_prvarg_template.hpp. On master: 44622 fails
(move 1, dtor 2) and the two-argument pair form gives move 2, dtor 4.

Repro (free template, master exe moves once; clang++ moves zero):

    struct Ev { int value; Ev(int n); Ev(const Ev&); Ev(Ev&&); ~Ev(); };
    template<class T> int free_tmpl(T x, int y) { return x.value * 1000 + y; }
    free_tmpl(Ev(7), next())   // clang: ctor 1 move 0 dtor 1

Previously recorded (still measured against the master exe): three approaches to the forwarding
wrapper did not change the count; the direct-ABI route above is the one that did.

The following extra copies are pre-existing; current counts match master and remain balanced
(dead == born), but clang++ -std=c++20 elides one copy in each case:

- A non-direct callee returning a conditional: 2,2,0,3 (clang 2,1,0,2).
- A non-direct callee returning a field: 1,2,0,2 (clang 1,1,0,1).
- A recursive callee: 3,2,0,4 (clang 3,0,1,3).
- `mk2(make(1))`: 2,1,0,2 (clang 2,0,0,1).

## Left after TERNELIDE (daa3a99f, 2026-10-30 box 7; Opus reviews, repros scratch/repro_keep/ternelide/ w/x/y/z .cb + .cpp twins, w.hpp)
- Same-class prvalue conditional (and plain prvalue) into `gen<T>(...)`, `function<>` and `Lambda<>` calls: one extra move (indirect /
  generic call path has no elision). Counts balanced.
- Compound assignment with a conditional rhs, `a += (k ? C(1) : C(2))`, is outside the operand gate (not probed against clang).
- `use(C(f(D(1))))`: D destroyed right after f returns (clang: end of full expression). MS ABI: `use2(k ? C(1) : C(2), b)` with a
  throwing copy ctor for b - the first slot is already handed to the callee, an unwind there leaks it (same as `use2(C(1), b)`).
