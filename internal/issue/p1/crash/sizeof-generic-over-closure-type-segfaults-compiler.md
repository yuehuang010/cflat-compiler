# `sizeof(Box<function<int(int)>>)` SIGSEGVs the compiler

Filed 2026-08-05 by `fix/genfn-lowering` while probing neighbour axes of the closure-element
lowering. **Pre-existing**, measured IDENTICAL on master `8c5a860` and on the post-fix binary.

Severity: compiler SIGSEGV, **exit 139, zero output, no diagnostic of any kind**. Not the
`CompilerManager` state dump - nothing at all.

## Repro

```cflat
import "function.cb";
struct Box<T> { T item = default; };
extern int main() { printf("S=%d\n", (int)sizeof(Box<function<int(int)>>)); return 0; }
```

Both binaries: exit **139**, no output.

## What is and is not affected - measured, all on both binaries

| Program | Result |
|---|---|
| `sizeof(Box<function<int(int)>>)` | **139**, no output |
| `sizeof(Box<Lambda<int(int)>>)` | **139**, no output |
| `sizeof(list<function<int(int)>>)` | **139**, no output |
| `sizeof(function<int(int)>)` | compiles, exit 0 |
| `sizeof(Box<int>)` | `(2,42): unknown type 'Box<int>'` - a located error |

So it is not `sizeof` of a closure (that works) and not `sizeof` of a generic instantiation in
general (that has a located diagnostic). It is specifically an instantiation whose type ARGUMENT
is a closure type, thin or fat, and it does not matter whether the template is a user `struct` or
`list`. `Box<int>`'s "unknown type" says the `sizeof` operand is not resolved through the
generic-instantiation machinery at all, so the closure argument is probably being parsed or
encoded on a path that then dereferences something the non-closure argument leaves null.

## Fix direction

Not diagnosed. Find where the `sizeof` operand's type name is resolved and why a closure type
argument diverges from `int` there - the `int` arm produces a clean `unknown type`, so making the
closure arm reach that same arm is already a strict improvement over the crash. Whether
`sizeof(Box<T>)` should RESOLVE for any `T` is a separate feature question; the crash must go
either way, per CLAUDE.md's rule that an LLVM assert or crash reachable from plain source becomes
a proper compile-time error once diagnosed.

## Test coverage

None. Wants an `expect_error` leg once it has a diagnostic, or a value-asserting leg if
`sizeof` over an instantiation is made to work.

Related: [[interface-issue-queue]]
