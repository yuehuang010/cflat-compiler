# Debug assertion sweep leftovers

Remaining:

- `Test/errors/err_cpp_header_leaves_scope_open.cb` still asserts in
  `clang::IncrementalParser::Parse` (`Lexer must be EOF when starting incremental parse!`).
  Three focused boundary/EOF changes did not resolve it; stop rule reached for this assert.
- `Test/test_cpp_interop.cb` Debug compilation exceeded 900 seconds three times after the mangle
  fix. The first attempt stopped near line 3820; later attempts reached line 8694 in the pointer
  overload negative cases before timeout. The final attempt used the parser code restored to its
  original state. A 25-second sample could not inspect the process under this sandbox.
- C++ `template <class T = int> struct Gate`: `Gate<>` and `Gate<int>` are distinct types in overload selection
  (clang: same type). Pre-existing (T200 review). T254 (Haiku) root: the main-pass declaration generic branch
  (MainListener_Declarations.cpp ~1012-1070) and ForwardRefScanner.cpp ~243 build the bare name `Gate` for an
  empty `<>` instead of the canonical `Gate$int`; `Gate<> a; take(a)` -> "no overload matches",
  `Gate<int> b; Gate<> a;` -> "cannot find the type 'Gate'". A typeAliases entry in publishRequestSpelling and
  skipping the empty-list branch in ForwardRefScanner both changed nothing. Repros scratch/repro_keep/t254/.
Repro: run each Test/*.cb with x64/Debug/cflat -B (one at a time, gtimeout 900).
