# `if const` has no constant-folding path: global `const` and enum members are rejected

Filed 2026-07-19.

## Summary

`if const` accepts only what LLVM happens to fold while emitting: literal expressions, the
builtin macros (`__MACOS__`, `__PLATFORM__`), and the generic intrinsics (`is_unique(T)`,
`is_pointer(T)`). There is no real compile-time constant evaluator, so anything that requires
a load is rejected with "'if const' condition must be a compile-time constant expression"
even when its value is statically known.

This is a feature gap, not a crash. The crash variant is tracked separately in
[`if-const-global-condition-crash.md`](if-const-global-condition-crash.md).

## Repro

```cflat
const int SEVEN = 7;
const bool FEATURE = true;
enum Flags : int { OFF = 0, ON = 1 };

if const (SEVEN == 7)    { ... }   // rejected
if const (FEATURE)       { ... }   // rejected
if const (Flags.ON == 1) { ... }   // rejected

if const (1 + 2 == 3)    { ... }   // OK - literals fold
if const (__MACOS__)     { ... }   // OK - builtin macro
```

The global `const` declarations themselves are fine: `const int SEVEN = 7;` at file scope
compiles and reads correctly at runtime. It is a runtime global with a const qualifier, not
a compile-time constant.

## Second symptom: short-circuit operators do not fold

```cflat
if const (__MACOS__ || __WINDOWS__) { ... }   // rejected
```

Both operands are compile-time constants, but `||` / `&&` lower to branches with a phi
rather than folding, so the result is not a `ConstantInt`. Workaround today is a chained
`else if const`. This one is arguably the more annoying of the two, since it bites portable
core code that has no globals involved.

## Root cause

`ParseIfConstDeclaration` (and now `AppendIfConstMembers`) evaluate the condition by calling
`ParseExpression` and testing `llvm::dyn_cast<ConstantInt>` on the emitted value. That makes
"is this constant" mean "did LLVM's IRBuilder fold it during emission", which is a much
narrower question than "is this a constant expression".

## Fix direction

Give `if const` a real constant-expression evaluator that runs before/instead of emission and
knows about:
- initialized global `const` scalars (fold to the initializer),
- enum members,
- short-circuit `||` / `&&` over constant operands,
- the existing builtin macros and intrinsics.

Falling back to the current emit-and-dyn_cast path is acceptable for anything the evaluator
does not understand, but the evaluator must run first. The same change is the clean fix for
`if-const-global-condition-crash.md`, since a real evaluator never needs an insert block.

## Related

- `internal/issue/if-const-global-condition-crash.md` - the SIGSEGV variant.
- `internal/plan/unique-ownership.md` - member-scope `if const` (added 2026-07-19) makes
  `if const` load-bearing for core container ownership policy, which raises the value of
  fixing this.
