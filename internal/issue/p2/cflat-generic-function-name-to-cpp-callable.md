# Remaining callable-binding gaps

Summary: the ST1 generic-instantiation cases are fixed in Test/test_cpp_interop_bridge.cb 631-636.
T208 fixed the Lambda<Fn()> sret crash and the function-pointer call-argument move / return-ABI
mismatches. T514 fixed target-signature selection for overloaded names in field defaults and
array initializers, deduction of unique named functions into `auto`, overloaded `auto` refusal,
and owned-return spelling in bind diagnostics. The remaining gaps are:

- assigning a callable to an existing `std.function` (`a = add3;`) reports "has no assignment
  operator cflat can call"; only the declarator / constructor forms bind.
- review round 1 (n53_review1.md): a variadic function pointer spelling loses `...`; a plain
  move-parameter function into std.function double-cleans; an owning void* return into
  std.function crashes.
- (T208 review, pre-existing) `Lambda<std.string()>` / `function<std.vector<int>()>` spelled directly report "cannot find
  the type" (aliases work); a CFlat function with a `function<alias-of-C++-class()>` parameter is never defined (link fails).
