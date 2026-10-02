# Calling a Lambda<Fn()> that returns std.function crashes with SIGBUS

Summary: left over from the ST1 follow-ups. The generic-instantiation half (`dbl<int>` into
std::function / a deduced C++ callable, and the explicit-constructor Upconvert crash) is fixed
(Test/test_cpp_interop_bridge.cb 631-636). What remains, pre-existing on master: calling a
`Lambda<Fn()>` whose result is a `std.function` crashes at run time with SIGBUS.

Repro: scratch/repro_keep/st1_followups/lambda_maker_existing.cb, lambda_maker4.cb (clang twin
lambda_maker4.cpp prints 8 -5).

Root cause: not investigated.

Also seen (separate gaps, plain function names too, pre-existing on master):
- assigning a callable to an existing `std.function` (`a = add3;`) reports "has no assignment
  operator cflat can call"; only the declarator / constructor forms bind.
- a function name passed as a CALL ARGUMENT to a CFlat `function<int(int*)>` parameter skips the
  `move` modifier check the declarator applies (`int consume(move int* p)` binds; double delete).
- a `move T*`-returning function name passed as a call argument to a CFlat `function<T*(int)>`
  parameter crashes the compiler (internal compiler error).
- `auto p = add1;` / `auto q = dbl<int>;` report "the function 'p' is not known" at the call.
- review round 1 (n53_review1.md): a variadic function pointer spelling loses `...`; a plain
  move-parameter function into std.function double-cleans; an owning void* return into
  std.function crashes.
