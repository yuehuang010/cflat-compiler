# function<> / Lambda<> parameter types cannot carry pointee const

Summary: `function<int(void*, (const i8)*, i64)>` parses but the listener cannot resolve the parameter type, and the
function-type encoding ignores ConstMask, so a function value cannot declare a read-only pointer parameter. Const phase 3
kept one cast for it: core/network/tls.cb:347 (SSLWrite, loaded via dlsym, whose C prototype takes `const void*`).

Fix direction: carry the pointee ConstMask of each param/return in the function-type encoding and resolve the
whole-word spellings inside function<>/Lambda<> arguments. This changes function-type identity (like overloading on
pointee const) - needs a maintainer ruling on mangling first (const plan R1).
