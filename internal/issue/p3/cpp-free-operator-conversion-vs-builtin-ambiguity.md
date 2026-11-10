# Imported enum free operator resolution

Remaining T460 item after fixing the original free-operator ambiguity, hidden-friend visibility,
and global-namespace lookup cases.

## Repro

`Test/library/cpp_interop_scoped_enum.h` defines `cppi_scope::PlainN` and free operators on other
types in the same namespace. The expression below must use the built-in enum arithmetic and fold
to 3:

```cflat
import cpp "library/cpp_interop_scoped_enum.h";
enum T45Fold : int { Unscoped = cppi_scope.PN_TWO + 1 };
```

The C++ operator probe is `scratch/t460/enum_operator/m.cb` with twin
`scratch/t460/enum_operator/o.cpp`: an overloaded `operator+(E, int)` should produce 25. On the
measured master, it compiles but returns 5; clang++ -std=c++20 returns 25. Experimental enum
support in this run returned 25 but regressed the constant-expression repro above, so it was
withdrawn after three unsuccessful fixes.

## Root cause

`cflat/MainListener_Expressions.cpp`, `tryFreeOperator`: enum values have scalar LLVM types and no
`StructType` name. The current early return excludes them from free-operator lookup. When that gate
is widened, candidate binding must use the resolved enum identity (`lhsTypeName` / `rhsTypeName`),
not the empty `leftInfo.second` / `rightInfo.second` values. The empty source name is treated as a
wildcard by the class-operand binder, so unrelated class overloads are marked viable and produce a
false no-overload diagnostic instead of letting built-in enum arithmetic fold.

## Fix direction

Add enum operands to free-operator lookup while preserving their resolved enum identity in
candidate binding. Only send clang a wrapper request when an enum overload candidate can bind; a
built-in enum expression in a constant-expression context must stay on the native folding path.

## Opset measurement (2026-11-09)

The cited enum operator repro returns 5 on this worktree and master, while clang++ -std=c++20
returns 25. Three focused implementation attempts exposed distinct false candidates or lost
conversions in the related enum arithmetic cases; the fix was withdrawn after the stop rule.
This item remains active.
