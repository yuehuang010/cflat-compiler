# CFlat default-parameter temporaries: remaining caller lifetime

DEFCLS (daa3a99f, 2026-10-30 box 7) supports C++-class defaults on CFlat functions through a
default-param wrapper. Class defaults in these wrappers still die before the caller's full
expression ends: `wdefB(rv.mkB(1))` logs CFlat `b1 b5 ~b5 =1005 live=2 ~b1`, while clang logs
`b1 b5 =1005 live=3 ~b5 ~b1`.

Root cause: the wrapper evaluates the default expression in its own stack frame and flushes the
owned temporary before returning. Keeping the destructor obligation in the wrapper cannot extend
the object's lifetime into the caller. A caller-owned destination or call-site lowering is needed.
Three destination-slot attempts did not move the destructor past the wrapper return; leave this
item open rather than masking it with a leaked object.

Native defaults remain intentionally unchanged: a native `N` default temp can leak, explicit
`fRd(mkN(1), N(5))` can destroy `n5` twice, and native/unique temps built inside a C++ class
default can leak. They are outside this class-temporary timing item.

Related: omitted defaults on C++ functions use CDEFARG caller-slot initialization. Defaults that
fail declaration-identity re-read still use the forwarding wrapper, with its existing early-destroy
behavior.

Round 3 residue: C++ class-typed, pointer/nullptr, string-literal, and nested-brace elements in
C++ class parameter defaults are refused with a clean diagnostic. Three shared-helper attempts
preserved class storage but still trapped or produced an extra constructor, and did not match
clang for nullptr/string arguments. Scalar non-null non-string brace elements remain supported;
the refused shapes match master's prior clean-refusal behavior. Earlier-parameter brace defaults
remain accepted because scalar `int f(int a, int b = a)` is also accepted by CFlat.
