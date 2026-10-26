# Loop call temporaries used through member pointers leak

Summary: A temporary owning object returned by a call is not destroyed when its member pointer is passed to a function in a loop.
Repro: `use(mk()->heap)` and `use((mk())->heap)` in a loop.
Master vs expected: master runs zero temporary destructors over ten iterations; expected one destructor per iteration.
Found: T426 r1 opus review, 2026-10-24 box, PRE-EXISTING
