# A typedef name inside a function<> signature resolves wrongly

Found 2026-11-01 by fnlam Opus review 2, PRE-EXISTING (master same).
Repro scratch/repro_keep/fnlam/rev_fnlam_opus_typedef_sig.cb (`typedef int myint;`):
- `function<function<int(myint)>()> get = g2;` -> "cannot choose which 'g2' to use".
- `int u(function<int(myint)> f)` called -> link error "undefined symbol __u2$..." (definition and call mangle the alias differently).
Fix direction: resolve typedef / using aliases to the canonical type before encoding a function-type signature component (both
ParseDeclarationSpecifiers copies + the closure/thin encoder), so the definition, the call and the binding agree.
