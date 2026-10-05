# C++ enum leftovers after the errc conversion fix: cast operands, optional<enum> assignment, std.numbers message

Found by the T5 review (Opus), 2026-10-02 (probes scratch/repro_keep/t5_rev/). All refused, none wrong-valued.

1. `mk == (std.errc)22` (cast enum operand): clang 1; master silently 0; now refused with an INTERNAL wrapper
   name (`no overload of '__cflat_free_d84ac3ee5e7422e9'`) - every user-facing surface must demangle
   (invertible-mangling ruling). `std.error_condition c = std.errc(22);` and `(E)3` copy-init refused (clang accepts).
2. `std.optional<std.errc> o; o = std.errc.invalid_argument;` refused; copy-init of the same works; clang accepts.
3. `o = std.numbers.pi` (refused on master too, clang accepts) now reports a misleading class-assignment message:
   the `starts_with("std.")` check in the enum path sends it down the class-assignment route.

Fix direction: route cast/functional-cast enum values through the same converting-ctor path as enumerators;
demangle wrapper names in the no-overload message; tighten the enum classification to real enum constants.
