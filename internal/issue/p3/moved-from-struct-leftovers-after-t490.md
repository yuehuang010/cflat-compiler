# Moved-from struct leftovers after T490

T490 (2026-10-24) made a moved-from plain CFlat struct (user dtor, no owning fields) dead at scope exit. Still open:

- (T418, master too) A by-value C++ class parameter consumed with `_ = move t` runs its dtor 3 times for 2 objects
  (`cfm(rettw())` -> stats 10130, clang 10120). The T490 r1 attempt (skip the release for parameters) fixed the count but
  deferred the release to function exit (stats inside the callee 10001 vs master 10010), unlike `_ = move` on a C++ local;
  backed out. Fix: keep the immediate release and disarm the parameter's later cleanup.
  T499 (2026-10-25) tried "callee destroys the parameter when its body always consumes it, caller skips": correct for
  direct calls, but two rounds failed on indirect calls - a function value keeps the first target's contract
  (`fp = A; if (c) fp = B;` leaks, a typed function<int(Val)> parameter destroys twice: 10240 vs clang 10120), and lambdas /
  generic function values skip the caller destroy without a callee destroy (leak). Backed out; master behaviour kept.
  The contract must be readable by every caller (part of the function TYPE / ABI), or callee-destroys only for functions
  never taken as a value. Conditional consumes (`if`, early return, loop, recursive: 20030 vs clang 20020) need the
  same contract plus the callee's conditional drop flag. Repros: scratch/repro_keep/t499/ (main checkout).
- (master too) `take(move b.inn)` (moving a struct field out) destroys `inn` twice.
- (master too) `Bx b = { inn = move a };` runs one extra dtor on an empty default value.
- Scope: structs with a raw-pointer or `int[]` view field are excluded from T490 (cautious; C-style `~Buf(){ free(data); }`)
  and still get 2 dtor runs after a move. Ruling question: does "no owning fields" cover raw pointers?
Repros: scratch/repro_keep/t490/ (main checkout).
Found: T490 r1 review (2026-10-24).
