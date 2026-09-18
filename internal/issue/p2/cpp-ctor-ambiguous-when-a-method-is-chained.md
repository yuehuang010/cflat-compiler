# A C++ constructor call becomes ambiguous when a method is chained onto its result

## Summary

`c10.TensorOptions(c10.ScalarType.Float)` compiles on its own, but the SAME constructor call
with a fluent method chained onto the temporary is rejected:

```
probe3.cb(6,26): C++ class 'c10.TensorOptions' matches more than one constructor overload
        c10.TensorOptions g = c10.TensorOptions(c10.ScalarType.Float).requires_grad(true);
```

The error points at the CONSTRUCTOR, not at `requires_grad`. So the set of constructor
candidates the resolver considers depends on what else the expression requests from the class -
requesting `TensorOptions::requires_grad(bool)` materialises additional constructor overloads
(most likely `TensorOptions(caffe2::TypeMeta)` or the variadic `template<typename... Args>
TensorOptions(Args&&...)`), and the resolver then reports a tie instead of preferring the exact
match `TensorOptions(ScalarType)`. Found 2026-09-16 dogfooding libtorch
(scratch/dogfood/torch/linreg.cb); this is the spelling every libtorch tutorial uses for a
requires-grad leaf, so it is the first line a real user writes.

## Repro

Two one-screen programs, identical except for the chain (both under scratch/dogfood/torch/):

probe2.cb - COMPILES AND RUNS:
```c
import cpp "torch/torch.h";
extern int printf(const char* fmt, ...);
extern int main()
{
    c10.TensorOptions f = c10.TensorOptions(c10.ScalarType.Float);
    c10.TensorOptions l = c10.TensorOptions(c10.ScalarType.Long);
    printf("hd=%d %d\n", (int)f.has_device(), (int)l.has_device());
    return 0;
}
```

probe3.cb - FAILS:
```c
    c10.TensorOptions g = c10.TensorOptions(c10.ScalarType.Float).requires_grad(true);
```

Workaround: split the chain and mark the tensor afterwards instead -
`at.Tensor w = torch.zeros(sizes); w.requires_grad_(true);`. (Assigning the chain result to a
named local in two statements was not tried; the practical workaround was to avoid
TensorOptions entirely.)

## Root cause

Not investigated, but the probe pair pins it down: the candidate set for a C++ constructor is
request-set dependent, and the constructor overload ranking does not prefer an exact parameter
match over a match reached through a user-defined conversion. `ScalarType` -> `TypeMeta` is
exactly such a conversion, and `TensorOptions(ScalarType)` should win outright.

Related but distinct: internal/issue/p3/cpp-implicit-ctor-conversion-at-call.md (a needed
implicit conversion is NOT offered). This one is the opposite failure - a conversion IS offered
where it should have been outranked - and the two should be fixed together, since both come
from the same missing conversion-ranking step.

## Fix direction

Give the C++ constructor overload resolver clang's ranking: exact / qualification / promotion /
standard conversion beat a user-defined conversion, and only a genuine tie at the same rank is
an ambiguity error. Make the candidate set independent of which members the expression
happens to request, or at least make the ranking insensitive to it.

Regression coverage: an in-repo fixture class with both `Foo(Enum)` and `Foo(Wrapper)` where
`Wrapper` has a non-explicit `Wrapper(Enum)`, plus a fluent `Foo& flag(bool)`; assert that
`Foo(E::a)` and `Foo(E::a).flag(true)` both resolve to the same constructor.

## Measurements 2026-09-18 (gated Phase A - DID NOT REPRODUCE; nothing fixed, no compiler change)

Binary: worktree x64/Release/cflat built from master 7897acf2 (the `explicit`-aware
SelectCxxConstructor(allowExplicit) commit), `--init-local` warm.

### 1. The ORIGIN repro no longer produces this error

`scratch/ccr_torch3.cb` is probe3.cb verbatim, run against the same homebrew libtorch 2.14.0
(`--c-include /opt/homebrew/opt/pytorch/include ... --check`, 12 min). Exit 1, but the
diagnostic is NOT the filed one - zero occurrences of "more than one constructor overload":

```
ccr_torch3.cb(6,26): no overload of 'requires_grad' matches the given arguments.
  Call arguments (2):
    [0] c10.TensorOptions <unnamed>
    [1] i1 <unnamed>
  Candidates (61): ... requires_grad(c10.TensorOptions*, std.optional<bool>)
  Argument mismatch detail (single resolved candidate: requires_grad):
    [0] arg=c10.TensorOptions  param=c10.TensorOptions*
    [1] arg=i1                 param=std.optional<bool>
```

So `c10.TensorOptions(c10.ScalarType.Float)` now resolves in BOTH spellings; the chain fails one
step later, at `requires_grad`, because `bool` is not converted to `std::optional<bool>` at a
member-call argument. That is the twin p3 issue
(internal/issue/p3/cpp-implicit-ctor-conversion-at-call.md), not this one.

### 2. Twelve in-repo cells, none reproduce the ambiguity

One header with variants A-L, probe pairs `Opts(Scalar.F)` vs `Opts(Scalar.F).flag(true)`:
exact+converting ctors; converting ctor only; variadic template ctor; three sibling
`enum class` ctors (the TensorOptions Layout/ScalarType/MemoryFormat shape); int8_t-backed
enums; SFINAE'd `T&&` and variadic ctors; fluent method returning `Opts&`, `Opts` by value and
`const Opts&`; every member out-of-line in a .cpp (so the chain forces the definition-enabled
re-extraction in TryBindRefusedCxxMember); and the enum re-exported into the class's namespace
by a using-declaration. Every cell compiled and ran with the correct value in both spellings.

Measured facts that contradict the filed root cause:

- The candidate set is NOT request-set dependent in-repo. Run with `--check -v`, the plain and
  the chained spelling print the SAME single `[verbose]   ctor candidate ...` line
  (LLVMBackend_CInterop.cpp:13592 emits one line per surviving candidate).
- `IsScopedEnumMatch` (LLVMBackend_StateAndImports.cpp:869) keeps sibling `enum class` ctors out
  of the set entirely: a scoped-enum argument matches only the same scoped enum. For a tie the
  argument would have to lose its scoped-enum identity first.
- `TensorOptions(caffe2::TypeMeta)` cannot be the second candidate: TypeMeta's only single-
  argument ctors are copy, move and `explicit TypeMeta(uint16_t)` (c10/util/typeid.h:354), and
  an `explicit` one is not offered as the implicit conversion since 7897acf2.
- A constructor TEMPLATE is never bound as a candidate, even when explicitly instantiated
  (`template ccr::OptsJ::OptsJ(ccr::Scalar8&&);` in the header still leaves one candidate), so
  the variadic `TensorOptions(Args&&...)` hypothesis has no in-repo support either.

### 3. The reproducible in-repo shape is the p3 twin, not this issue

Cell H mirrors TensorOptions including the real fluent signature and reproduces the CURRENT
libtorch failure exactly, same diagnostic shape:

```c
struct Meta { int m; Meta(Scalar8 s); };
struct OptsH {
    int k; bool f;
    OptsH() noexcept;
    OptsH(Layout8 l); OptsH(Scalar8 s); OptsH(MemFmt8 m); OptsH(Meta m);
    template <class T, class = std::enable_if_t<std::is_same_v<std::decay_t<T>, Dev>>> OptsH(T&&);
    template <class... A, class = std::enable_if_t<std::is_constructible_v<Dev, A&&...>>> OptsH(A&&...);
    [[nodiscard]] OptsH flag(std::optional<bool> b) const noexcept;
};
```

`ccr.OptsH(ccr.Scalar8.F)` compiles; `ccr.OptsH(ccr.Scalar8.F).flag(true)` gives
"no overload of 'flag' matches the given arguments ... [1] i1 <unnamed> ...
flag(ccr.OptsH*, std.optional<bool>)". Replacing the parameter with a plain `bool` (cell I)
makes both spellings compile and run.

### Recommendation

Do not fix this issue as filed - re-verify the symptom before spending on it. The ranking work
it describes has no failing case today. The libtorch line it came from is now blocked by the p3
twin, which does have an in-repo repro (cell H above); that issue should carry this header
shape. Probe corpus (gitignored): scratch/ccr_*.cb, scratch/ccr_ctorchain.h,
scratch/ccr_ctorchain_oo.{h,cpp}, scratch/ccr_alias.h, scratch/ccr_matrix.md.
