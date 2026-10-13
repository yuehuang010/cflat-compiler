# Mixed-class `?:` argument: overload viability leftovers after T147

NARROWED by T147 (2026-10-12): a free-function call `f(c > 0 ? a : b, 7)` with f(A*, int*) and f(I, int) now drops candidates that
cannot take the other arguments and boxes into f(I, int) (122). Still refused (master refuses too; refusal is the safe direction),
clang C++20 interface-pointer controls accept. Repros scratch/repro_keep/t147/:

1. Receiver calls (method / UFCS): `x.f(c ? a : b, 7)` - `arguments` includes the receiver but the deferred-join index list holds only
   explicit arguments, so the size check always declines (cflat/MainListener_Expressions.cpp ~1929, MainListener_PostfixExpression.cpp
   ~7513). Carry the receiver offset through viability filtering and the argument update. method.cb, ufcs.cb.
2. Two deferred joins in one call: f(A*, A*, int*) / f(I, I, int) called `f(c ? a : b, c ? a : b, 7)` - each join is tested against the
   other raw join and the interface candidate is dropped. Resolve pending joins together (ordinary args first, interface feasibility
   without emitting boxing IR during probing). twojoins.cb (expected 51).
3. Function-pointer callee with an interface parameter: fp.cb (expected 122).
4. Primitive pointee vs string literal: `void* p = c ? ip : "abc"` (int* arm) accepted; clang refuses (T147 only made class pointees
   refuse). primitive_literal.cb.
5. Literal/literal ternary into a std::string parameter or a converting ctor refused; clang 97. stdstring.cb, ctor.cb.
