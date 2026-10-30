# Mixed-class C++ conditional used as a value, return or argument is refused (clang accepts)

Status (r5): accepted only as the sole argument of a single-parameter CFlat callee (the sink; same-arity
CFlat overloads such as tq(PB) + tq(int) are fine, but any same-name free overload with MORE parameters
refuses it), or as a decl/sret value with no conversion of a by-value parameter arm. Everything else
is refused with master's "ternary branches have incompatible types" or "cannot return C++ class".

## Still refused (clang accepts)
- Non-sink call args: `take2(1, k ? mkA(11) : mkB())` (c05); method/C++ callee args (c06, c15).
- Operand of a binary/unary/cast op: `take(k ? mkA(11) : mkB()) * 2 + ...` (c24, c37).
- Sink with a dangling view: `take2(1, k ? mkAV(11) : mkV())` (c30, c31).
- Defaulted trailing parameter (sink would forward a copy): `tq(k ? mkA(11) : mkNB())` (S2).
- Nested decl: `PB b = k ? mkA(11) : (j ? mkA(12) : mkB());` (c14).
- Overloaded callee: `ov(k ? mkA(11) : mkB())` with two overloads (c07).
- Return of a converting by-value parameter lvalue arm: `return k ? a : mkNB();` (S3, c25). Local lvalue arm (c34) stays accepted; matches clang.

## MS sink lifetime (S1): fixed in wrapper text, Windows not run
The MS callee-destroy wrapper builds B by placement new inside the sink call's single
full-expression, so the converted-from arm lives through the call. Checked (clang MSVC target):
-fsyntax-only, and IR (one B dtor, in the callee; both A dtors after the indirect sink call).
Legs V13-V15 use one value on both ABIs.

## F1 blocker: thunk-arm temp lifetime
A prvalue arm A from a CFlat function is a wrapper-local temp in `__cflat_ternary_arm_*`, destroyed
at wrapper end. The owned-temp ledger cannot hoist it, so B(A) reads a dead A. clang keeps A to the end of the full expression. Const-ref A needs per-kind ctor selection.

## Fix direction
(a) Caller-owned producer storage: emit B's converting ctor from CFlat with a pointer to a
caller-owned A, so the temp outlives the wrapper.
(b) Multi-arg sink with captured args (r2 idea), then drop the operator and overload refusals.
Check ctor/dtor sequences against clang++ on both ABIs (MSVC destroys by-value params in callee).
