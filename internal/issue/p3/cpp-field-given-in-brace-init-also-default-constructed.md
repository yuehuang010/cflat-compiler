# C++ class field given in a brace init is also default-constructed first

T73 review 1 (2026-10-07); master same for a class with a user-declared default ctor. A CFlat struct
with a C++ class field `m`: `H b = { m = mm };` default-constructs `m`, destroys it, then
copy-constructs from `mm` - 4 ctors / 4 dtors where clang++ C++20 gives 2 / 2 (balanced, so no leak,
but an observable extra ctor/dtor pair and wasted work). Fix direction: the brace-init field
default pass must skip fields the initializer list names, and construct those in place.
Probes: scratch/repro_keep/t73_rev/p2.cb, p3.cb.
Also from that review (nit): MainListener_Aggregates.cpp ~185/~1505/~5175 drop the
TryBindCxxImplicitDefaultCtor error, so an ambiguous default ctor gets the generic "no default
constructor cflat can call" text.
