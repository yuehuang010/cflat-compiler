# A free C++ function parameter whose spelling merely CONTAINS `std::function<` is retyped as that std.function

Found 2026-09-17 by the std::function member/return review (macOS arm64, Release, master 97b810fe). Pre-existing on master; deliberately left unchanged by fix/cpp-std-function-member-return, which anchors the match at top level for member parameters and all returns but not for free-function parameters.

## Summary

The free-function parameter retype in RegisterCSignatures (cflat/LLVMBackend_CInterop.cpp, SplitStdFunctionSpelling) matches `std::function<` anywhere in the parameter spelling. `inline int use_vec(std::vector<std::function<int(int)>> fs)` therefore binds as if it took `std.function<int(int)>`: the call compiles and prints garbage (-292 measured, review probe scratch/rv/lo5.cb) instead of a clean refusal. Members and returns now use IsTopLevelStdFunctionSpelling and refuse this shape cleanly; the free-parameter path is the remaining asymmetry.

## Fix direction

Gate the free-parameter retype on IsTopLevelStdFunctionSpelling too, and add a refusal leg to Test/errors/err_cpp_std_function_nested.cb for a free parameter of vector<function>. Check that no fixture leg depends on the loose match (grep `std::function<` parameters in Test/library/cpp_interop*.h wrapped in another template).

Suggested bucket: p2 (silent miscompile on a clean compile, narrow shape).
