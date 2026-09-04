# `bool` default argument from an integer literal fails module verification

Bucket: needs a RULING (accept-vs-reject), then full mode; not batchable.
Filed 2026-09-04 from the q04 batch review (out of scope there: not a lowering fix).

## Summary

`void f(bool b = 2);` then `f();` pushes the default value with no conversion to the parameter
type (cflat/MainListener_Statements.cpp ~3406), so codegen emits `call @f(i8 2)` into an i1 slot
and the verifier rejects the module ("Call parameter type does not match function signature!")
with no `LogError`. A positional `f(2)` is REJECTED by overload resolution, so the two paths
disagree about whether an integer converts to `bool` at a call.

## Fix direction

Ruling: is integer -> bool implicit conversion at a call legal (then the default-argument path
and overload resolution both accept and route through `CoerceToBoolCondition`, the door q04
added for casts / assignment / brace-init), or illegal (then the default-argument site rejects
with the same "no overload" / "cannot convert" text)? Either way the verifier failure must
become a `LogErrorContext` or a conversion. Sibling of `p2/narrow-param-call-arg-skips-truncation-verifier-failure`
(same disagreement between overload scoring and argument materialisation); rule both at once.
