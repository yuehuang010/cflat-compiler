# A block-scope enum type name does not shadow an outer variable of the same name in value lookup

Summary: `const int n = 9; ... { enum n : int { A = 3 }; int v = n + 1; }` compiles and yields 10; clang rejects
the C++ twin ("'n' does not refer to a value"). Codegen name lookup ignores local type names. Pre-existing on
master; the const-fold helper was fixed to shadow in const phase 1 round 4, plain value lookup was not.

Fix direction: identifier resolution (codegen lookup order local/param, implicit-this field, global, function) must stop
at a local type of that name in an inner scope and report "'n' names a type, not a value".
