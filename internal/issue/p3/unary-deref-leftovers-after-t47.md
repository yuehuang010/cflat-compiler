# Unary `*` leftovers: fixed-array C++ record, implicit pointer conversion, plain CFlat struct

Found by T47 review 4 (2026-10-09). All three are pre-existing on master 608183756. T47 added a refusal for `*` on a C++ record VALUE ("indirection requires a pointer operand"). It is narrowed to C++ records, because template-substituted pointer fields (`function<T>**`) carry no Pointer bit.

1. `*arr` on a fixed array of C++ records: master fails LLVM verification, and T47 gives the new diagnostic. clang decays the array and runs (0). Repro: scratch/repro_keep/t47/rev_t47_r4_array.cb/.cpp.
2. `*c` where the C++ class has an implicit conversion to a pointer (`operator C*()`): refused on master and post; clang converts and runs (0). Repro: scratch/repro_keep/t47/rev_t47_r4_convert.cb/.cpp.
3. `*s` on a plain CFlat struct value: LLVM verification failure on both, where it should be a LogError like clang. Repro: scratch/repro_keep/t47/rev_t47_r4_plain.cb/.cpp.

Fix direction:
- Generalize the T47 guard (MainListener_Expressions.cpp, unary '*', `derefTv`) to every non-pointer value, after fixing the template-substitution representation, so substituted pointer fields carry Pointer / PointerDepth. test_function_ptr.cb is the case that broke the first general attempt.
- Array operands decay to an element pointer.
- A conversion operator to a pointer is tried before refusing.
