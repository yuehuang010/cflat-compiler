# Debug assertion sweep leftovers

Remaining:

- `Test/errors/err_cpp_header_leaves_scope_open.cb` still asserts in
  `clang::IncrementalParser::Parse` (`Lexer must be EOF when starting incremental parse!`).
  Three focused boundary/EOF changes did not resolve it; stop rule reached for this assert.
- `Test/test_cpp_interop.cb` Debug compilation exceeded 900 seconds three times after the mangle
  fix. The first attempt stopped near line 3820; later attempts reached line 8694 in the pointer
  overload negative cases before timeout. The final attempt used the parser code restored to its
  original state. A 25-second sample could not inspect the process under this sandbox.
Repro: run each Test/*.cb with x64/Debug/cflat -B (one at a time, gtimeout 900).
- 2026-11-01 sweep (macOS Debug, every Test/*.cb except test_cpp_interop): `Test/test_operators.cb`,
  `Test/test_collection_leaks.cb` and `Test/errors/err_ternary_nesting_depth.cb` overflow the 8 MB main-thread
  stack (`___chkstk_darwin` in `MainListener::ParsePostfixExpression` <- `ParseUnaryExpressionImpl`) before the
  `kMinStackHeadroomBytes` (256 KB non-Windows) recursion guard fires: unoptimized clang frames are far larger
  than Release ones. Release is fine. Fix direction: a Debug-only headroom like the Windows one, or run the
  compile on a thread with a larger stack - check test_operators still compiles under Debug either way.
- Tried 2026-11-01: a macOS `LINKER:-stack_size,0x4000000` (64 MB main stack, like Windows /STACK) fixes the Debug
  overflow of test_operators / test_collection_leaks, but REJECTED: err_ternary_nesting_depth is calibrated to the 8 MB
  stack (Release then compiles the 100-deep ternary and the expect_error fails), and Debug still SIGBUSes inside ANTLR's
  unguarded `TerminalNodeImpl::getText` recursion. A Debug-only fix must keep Release acceptance identical.
