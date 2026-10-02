# Range-for over a C++ class: begin/end member-name lookup misses enums and nested types; count/get clients pay a failed ADL probe

Found by the ST6 round-2 Sol review (landed with ST6).

1. Accepts invalid (P3): [stmt.ranged] uses r.begin()/r.end() when member lookup finds BOTH names, whatever their kind or access. A class whose `begin`/`end` are an enumerator or a nested type is accepted by CFlat (iterates via namespace begin/end, sum 6); clang rejects. Master rejects too. Repro: scratch/repro_keep/st6_followups/EnumBoth.cb, TypeBoth.cb, PrivateTypes.cb + member_names.h (+ .cpp oracles). Fix: CxxClassHasMemberNamed (LLVMBackend_CInterop.cpp) searches only methods, fields, static variables and function templates - use complete C++ member-name lookup (enumerators, nested types, private members); consumer MainListener_Statements.cpp range-for.
2. Cost (P3): a count/get range client issues one failed ADL begin wrapper request, cold and warm (~0.5 ms, +3 clang decl parses). Repro: counted.cb + new.h (oracle counted.cpp prints 6). Fix: skip / negative-cache the ADL probe when the class has no associated free begin/end candidates.
