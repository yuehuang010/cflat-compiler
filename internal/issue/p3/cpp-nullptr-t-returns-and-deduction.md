# std::nullptr_t leftovers: returns, const& parameters, deduction, defaulted parameters

Found by ST3 round 3 (all fail on master too): a C++ function returning `std::nullptr_t` (or an alias of it) cannot be bound; a `const std::nullptr_t&` member parameter is refused; `nullptr` into a deduced template parameter deduces void* (clang: nullptr_t); a defaulted nullptr_t parameter is refused. Probes: the ST3 round-2 review corpus (t_nil_deduce, t_nil_member_deduce, null_results.jsonl) - copied to scratch/repro_keep/st3_followups/.

Model: ST3 added `IsCxxNullptrT` on TypeAndValue (set by the C++ type mapper and std.nullptr_t aliases) and the guard in LLVMBackend_Overloads.cpp; extend the mark to return types, const& parameters and template deduction spelling (spell `decltype(nullptr)` for a marked argument).
