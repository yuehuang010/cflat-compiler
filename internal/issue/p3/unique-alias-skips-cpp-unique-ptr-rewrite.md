# p3: `using UQ = unique;` then `UQ<CppClass>` skips the R5 rewrite to std::unique_ptr

Pre-existing; found by the T383 review (2026-10-21). Repros: scratch/repro_keep/box1021/t383_rev/i_unique_alias.cb (fails),
i2_unique_direct.cb (same program spelled `unique<uqp.W>` - returns 21).

Bridge ruling R5 (2026-09-23): the `unique` keyword on a C++ type means std::unique_ptr. Through a generic-base alias of `unique`, the
rewrite does not fire and a later `move p` is refused: "'p' is not a 'std.unique_ptr$uqp.W' value that can be moved".
Direction: resolve the alias to its base (`unique`) BEFORE the R5 C++-pointee rewrite in both type-parse copies (ParseDeclarationSpecifiers
in ForwardRefScanner + MainListener). If aliasing the `unique` keyword is not meant to be legal at all, refuse the alias declaration
instead (maintainer may prefer that - `unique` is a keyword-like library type).
