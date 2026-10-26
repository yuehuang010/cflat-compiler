# unique alias function parameter lowers to raw pointer

Summary: `unique<T>` in a function type is lowered as a raw pointer, while a standard-library spelling is unresolved.
Repro: `function<int(unique<uqp.W>)>` and `function<int(std.unique_ptr<uqp.W>)>`.
Master vs expected: first lowers to `int(uqp.W*)` and cannot bind a `std.unique_ptr`; second reports `cannot find the type`.
Found: T473 Opus r1 review, 2026-10-24 box, PRE-EXISTING
