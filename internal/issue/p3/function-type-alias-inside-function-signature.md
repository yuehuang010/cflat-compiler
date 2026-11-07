# A function-type alias cannot be used inside a function<> signature

Found 2026-11-03 by the fntype review (box 10), PRE-EXISTING.

`using FI = function<int(int)>; int user(function<int(FI)> k) { ... }` -> "cannot find the type 'FI'"; the typedef form
(`typedef function<int(int)> FI;`) -> "cannot find the type '<c_fn_ptr>'". Expected: FI expands to its function type as
a signature component, like any other alias (landed 2026-11-03 for typedef / using aliases of non-function types).

Fix direction: the signature-component resolver (ResolveSigComponentConst*, both passes) must expand a function-type
alias via FindOrRequestFunctionTypeAlias into a nested function-pointer component.
