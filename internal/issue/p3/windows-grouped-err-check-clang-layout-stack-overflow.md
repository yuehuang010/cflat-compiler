# Windows: grouped --check of a C header + C++ header error fixture overflows the stack in clang record layout

Found at the mnulljoin Windows gate (2026-11-01). test_err_1 (`--check ... @err_group_0_of_8.rsp`) crashed with exception 0xC00000FD in
`clang::ASTContext::getASTRecordLayout` (64+ frames recursing through the VBaseInfo DenseMap) while compiling
err_lambda_nonnull_pointer_global.cb, which imported Test/library/c_variadic_fnptr.h (C) then Test/library/cpp_interop_fnptr.h (C++).
Mac passes the same file standalone and in test.sh. Other fixtures import cpp_interop_fnptr.h alone and pass on Windows.
Repro (pre-workaround fixture): scratch/repro_keep/mnulljoin_win_overflow_fixture.cb; Windows log C:\source\cflat-gate-logs\mnulljoin.test.log.
mnulljoin worked around it by dropping the C++ import (CPPFN_ZERO legs use a C macro T311_ZERO).
Next: run the fixture standalone on Windows (and grouped) to tell a real layout cycle (C and C++ TU sharing a record) from thread-stack
depth in the grouped runner; a user-reachable crash needs a located error or a fix.
