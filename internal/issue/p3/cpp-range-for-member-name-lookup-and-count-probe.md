# Range-for over a C++ class: count/get clients pay a failed ADL probe

Found by the ST6 round-2 Sol review (landed with ST6). The begin/end member-name lookup part
(enumerators, nested types, private names) is fixed on fix/t328-rangefornames; only the probe cost remains.

1. Cost (P3): a count/get range client issues one failed ADL begin wrapper request, cold and warm (~0.5 ms, +3 clang decl parses). Repro: counted.cb + new.h (oracle counted.cpp prints 6). Fix: skip / negative-cache the ADL probe when the class has no associated free begin/end candidates.
