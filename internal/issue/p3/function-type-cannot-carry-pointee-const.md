# function<> / Lambda<> parameter types cannot carry pointee const

Summary: `function<int(void*, (const i8)*, i64)>` parses but the listener cannot resolve the parameter type, and the
function-type encoding ignores ConstMask, so a function value cannot declare a read-only pointer parameter. Const phase 3
kept one cast for it: core/network/tls.cb:347 (SSLWrite, loaded via dlsym, whose C prototype takes `const void*`).

Fix direction: carry the pointee ConstMask of each param/return in the function-type encoding and resolve the
whole-word spellings inside function<>/Lambda<> arguments. This changes function-type identity (like overloading on
pointee const) - needs a maintainer ruling on mangling first (const plan R1).

C side (2026-10-19, c10 Sol review): a C function-pointer global/field keeps no result or param pointee const either -
`extern const char* (*revFp)(void);` then `char* p = revFp(); p[0] = 'z';` compiles (clang -Werror refuses). Skipped at
MapRawGlobal (`IsFunctionPointer`, LLVMBackend_CInterop.cpp ~:5077); same function-type encoding gap, same ruling needed.

RULED 2026-10-19 (maintainer): function types carry pointee const with EXACT-MATCH identity. Each param/return pointee
ConstMask is part of the function type (function<>, Lambda<>, C function pointers, mangled like generic args); no
qualification conversion between function types that differ only in pointee const (C/clang rule:
`int(*)(const char*)` and `int(*)(char*)` are distinct, assigning one to the other is refused). Unblocks this issue and
the C fn-pointer case above.
