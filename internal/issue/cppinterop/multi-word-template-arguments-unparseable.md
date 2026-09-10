# Multi-word template arguments do not parse

Found 2026-09-09 by the std header coverage spike ([`std-header-coverage-spike.md`](std-header-coverage-spike.md), gap 8).

## Repro

```cflat
import cpp "chrono";
extern int main() { std.chrono.duration<long long> d = default; return 0; }
```

    error: cannot understand the code at 'extern int main(){ std.chrono.duration<long long'

A parse error, not a type error - the second word of the argument ends the template argument list.

## Impact

Every `chrono` duration spelled directly, and any C++ specialization whose argument is a multi-word
C type: `long long`, `unsigned int`, `unsigned char`, `unsigned long long`, `long double`. The
alias spellings (`std.chrono.milliseconds`) dodge the parse but land on
[`constrained-template-constructor-overload-resolution.md`](constrained-template-constructor-overload-resolution.md)
instead, so there is currently no working way to name a duration.

## Fix direction

Accept a multi-word C type name as a template argument in the type grammar. Check whether the
CFlat single-word spellings (`i64` etc.) already reach the same specialization - if they do, this
is about accepting the C spelling a header author will actually write, not about new types.

Acceptance: the repro compiles and runs to exit 0; a specialization over `unsigned int` is asserted
alongside it in `Test/test_cpp_interop.cb`.
