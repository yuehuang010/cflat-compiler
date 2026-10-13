# A ternary of C++ class prvalues initializing a field is moved in, not built in place

Summary: `CppAssignHost h = { l = c ? cppas.Life(21) : cppas.Life(22) };` constructs the selected arm
as a temporary and move-constructs the field from it (ctors 1, moves 1, dtors 1 inside the scope);
clang++ -std=c++20 builds the selected arm directly into the field (moves 0, dtors 0 inside). Values
are right; one extra move + dtor per initialization.

Found: T117 round 4 (2026-10-11); pre-existing (master also moved, with an extra seed construction
T117 removed). Pinned at Test/test_cpp_interop.cb leg 3571 with the current counts.

Fix direction: when the field slot is fresh (T117's destinationIsFresh / EmitForeignCxxValueIntoSlot
intoFreshSlot), lower a ternary of prvalues as a branch that constructs each arm into the slot; then
tighten leg 3571 to moves 0 / dtors 0+1.
