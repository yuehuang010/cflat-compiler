# Typed null operands lose pointee / const identity in C++ free-operator ranking

A cast null passed as a binary-operator operand ranks as if it were a plain `T*` of the wrong pointee/cv:
`(int**)nullptr` picks the `int*` overload (CFlat 2, clang 4), `(const int*)nullptr` picks `int*` (2 vs 5),
`(const R*)nullptr` picks `R*` (7 vs 8). `char*`, a null `int*` local, `Ref*` and bare `nullptr` match clang.
Same on master 33866560 and after T8 (pre-existing; silent wrong overload, so p3 only because typed-null
operator operands are rare).

## Repro

scratch/repro_keep/t8_typednull/typed.cb + typed.hpp, clang twin typed.cpp (oracle `3 4 5 ... 8`).

## Root cause (GUESS)

MainListener TryBinaryOperatorOverload `makeArgument` carries pointer depth but not the cast's pointee type or
const qualification, so RankCxxConversionSequences sees a generic pointer.

## Fix direction

Carry the cast target type (pointee, depth, cv) of a constant-null operand into the argument, as a typed pointer
local already does.

Found by: T8 Luna verification round, fix timebox 2026-10-02.
