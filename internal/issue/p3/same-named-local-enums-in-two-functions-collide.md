# Two non-generic functions with a same-named local enum collide ("already defined")

Pre-existing on master; found by the T60 review (2026-10-06). `int a() { enum E { V = 1 }; ... }` and
`int b() { enum E { V = 2 }; ... }` in one file: the second is refused as already defined (C/C++: each
function scope has its own E). A local enum shadowing a file-scope enum of the same name in a non-generic
function is refused the same way (C/C++: the local one wins). T60 keys local enums per generic
instantiation only; extending the keying to every function scope (lambda -> enclosing function) fixes
both, but changes the IR names of every local enum. Repros: scratch/repro_keep/t60_rev/ (one.cb,
shadow2.cb). Also seen on master: `G g = E.V;` (different enum types) and `E e = 5;` pass --check
(C++ refuses both; C accepts) - needs a ruling on which rule CFlat follows.

Also (T60 review 2): two METHODS of the same struct (generic or not) each declaring a same-named local enum
fail with "already defined" (scratch/repro_keep/t60_rev/r2two.cb).
