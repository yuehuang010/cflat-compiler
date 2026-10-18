# C++ interop leftovers next to T334: cast global init, std.size(array), array-reference function pointer into a ctor

Found by the T334 round-1 review (2026-10-17); the same behaviour on master and the T334 branch. Repros (header probe.hpp) are in
scratch/repro_keep/box1017/t334_rev/ (global.cb, stdsize.cb, fnref.cb, each with a .cpp clang twin).
1. A global initialized by a cast to a C++ class, `char* gp = "abc"; rv.Box gv = (rv.Box)gp;`, is refused: "global variable
   initializer must be a compile-time constant". clang accepts it as dynamic initialization (value 97). Check the global/static
   storage ruling (memory: globals follow Rust, no exit-time destruction) before choosing between accepting and a specific refusal.
2. `int[3] p = {7,8,9}; std.size(p)` is refused: "C++ free function 'std.size' could not be bound (clang: no matching function
   for call to 'size')". clang returns 3 via `size(const T(&)[N])`. The array argument is spelled as a pointer, not as `int(&)[3]`.
3. `using RefFn = char(&(*)())[4]; inline RefFn getfn() { return &buf; }` (buf returns a static char[4] "abc"), then `rv.Fn v = rv.Fn(rv.getfn());`, where Fn's constructor calls f()[0].
   CFlat gives 0 and clang gives 97: the function pointer returning an array reference is called wrongly or binds the wrong value.
