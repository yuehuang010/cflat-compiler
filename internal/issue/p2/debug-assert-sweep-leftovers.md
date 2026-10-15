# Debug assertion sweep leftovers

Remaining:

- `Test/errors/err_cpp_header_leaves_scope_open.cb` still asserts in
  `clang::IncrementalParser::Parse` (`Lexer must be EOF when starting incremental parse!`).
  Three focused boundary/EOF changes did not resolve it; stop rule reached for this assert.
- `Test/test_cpp_interop.cb` Debug compilation exceeded 900 seconds three times after the mangle
  fix. The first attempt stopped near line 3820; later attempts reached line 8694 in the pointer
  overload negative cases before timeout. The final attempt used the parser code restored to its
  original state. A 25-second sample could not inspect the process under this sandbox.
- Global `float[] gz = 0.0;` (zero to array view at file scope) fails LLVM module verification with a fresh,
  never-initialised cache dir (passes with the local cache and with -B); same zero-to-view bug T200 fixed for
  locals, at the global-initializer site (T200 review, pre-existing).
- C++ `template <class T = int> struct Gate`: `Gate<>` and `Gate<int>` are distinct types in overload selection
  (clang: same type). Pre-existing (T200 review).
Repro: run each Test/*.cb with x64/Debug/cflat -B (one at a time, gtimeout 900).
