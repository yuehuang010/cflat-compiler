# Brace-list backing storage: shapes still built inside a thunk

Follow-up of the ctor-thunk brace fix (fix/brace-backing, 2026-09-27): array-reference and
std::initializer_list constructor targets now use caller-frame storage. Still built inside the thunk
(can dangle only when the callee keeps a view of the list):

1. An array / initializer_list constructor visible only through `using Base::Base` (not in the class's
   own constructor list) keeps the old spelling.
2. A brace aimed at a class-typed parameter whose own initializer_list ctor keeps a view
   (`Owner(IntArrayRef)` called with `{2, 3}`) builds that object inside the thunk.
3. `RequestCxxBraceFunction` (function-call brace wrapper) builds scalar lists inside its thunk; matters
   when the callee returns a view of the list.

Pre-existing, related: a trivially copyable class whose only ctor takes an initializer_list is "not
known" (ctor not bound); a mixed list `{v, v + 1}` is refused ("brace arguments must contain scalar
values of one type").

Fix direction: reuse the caller-side `E[N]` materialization in `RequestCxxBraceConstructor`
(LLVMBackend_CInterop.cpp) for each shape. Legs next to 7150-7155 in Test/test_cpp_interop.cb.

Found at final review (2026-09-27), still open:

4. A constructor template whose parameters carry a NON-TYPE template parameter outside the array
   extent, or a parameter pack (`requiresConstructorWrapper`), is not mirrored in the overload
   selector; the selector renames only `type-parameter-D-I`. Such a class beside a list constructor
   fails the wrapper with a relayed clang error rather than a CFlat message.
5. A selector candidate whose DEFAULTED parameter is template-dependent is spelled
   `= declval<tp_D_I>()`, which cannot deduce; clang reports it, CFlat does not pre-check.
6. Narrowing in a NON-backed list (function-call brace wrapper, class-typed elements) is still
   reported by clang through the wrapper and surfaces as "no overload ... matches"; the backed
   ctor path now names the element and target ("narrows to the C++ constructor's ...").
