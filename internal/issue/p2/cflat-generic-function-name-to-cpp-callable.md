# Remaining callable-binding gaps

Summary: the ST1 generic-instantiation cases are fixed in Test/test_cpp_interop_bridge.cb 631-636.
T208 fixed the Lambda<Fn()> sret crash and the function-pointer call-argument move / return-ABI
mismatches. The remaining gaps are:

- assigning a callable to an existing `std.function` (`a = add3;`) reports "has no assignment
  operator cflat can call"; only the declarator / constructor forms bind.
- `auto p = add1;` / `auto q = dbl<int>;` report "the function 'p' is not known" at the call.
- review round 1 (n53_review1.md): a variadic function pointer spelling loses `...`; a plain
  move-parameter function into std.function double-cleans; an owning void* return into
  std.function crashes.
- (T208 review, pre-existing) `Lambda<std.string()>` / `function<std.vector<int>()>` spelled directly report "cannot find
  the type" (aliases work); a CFlat function with a `function<alias-of-C++-class()>` parameter is never defined (link fails).
- Function-pointer spellings in bind diagnostics drop `move` on the return (both sides print 'Pl*(int)').
- (T287 review, pre-existing) a lambda literal with a `move` parameter RETURNED as a borrowed callable, or used to
  initialize a struct field of borrowed callable type, aborts rc 134 (double delete) on master and after T287: T287's
  FuncPtrValueOwnershipReason check covers call arguments, declarators and assignment binds, not returns or field
  initializers. Repros scratch/repro_keep/wt_t287/rev_t287_sol_tmp/{return_mismatch,field_mismatch}.cb.
