# Closure type nested in a function<> parameter type does not parse; bind diagnostic spells closures as <closure_fat_ptr>

Residue of p2 named-closure-return-fnptr-slot-mismatch (closed by T500 + T512).

- `int call(function<Lambda<int(int)>()> get) { return 0; }` fails its signature with
  "cannot find the type 'Lambda<int(int)>'" even with no binding at all (master and T512). A plain `Lambda<int(int)>`
  parameter works, and the same spelling works in a local declaration - only the parameter-type path loses it.
- The function-pointer bind diagnostic prints a closure-returning getter's type as `'<closure_fat_ptr>()'` instead of
  `'Lambda<int(int)>()'` (FuncPtrSigOfSymbol copies the LLVM struct name, cflat/LLVMBackend_Lookup.cpp ~1199).
Acceptance: value leg passing a getter through such a parameter in Test/test_function_ptr.cb; the diagnostic text in an
existing "cannot bind function" err leg names the Lambda<> spelling.
