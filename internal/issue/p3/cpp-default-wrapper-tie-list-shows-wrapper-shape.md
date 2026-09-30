# Ambiguous call through a C++ default-argument wrapper lists wrapper shapes, not declarations

Found by the R3 review round 2 (perf timebox 2026-09-29, on-demand default wrappers).

## Symptom
A C++ overload tie where one candidate reaches the call through a default-argument wrapper is refused
correctly (clang also rejects it), but the candidate list names the shortened wrapper shape instead of
the real declaration: `AmbHost.pick(4)` prints "cannot choose which 'pick' to call: pick(int), pick(int)";
another tie prints `r3.sv(const char*), r3.sv(const char*)`. Master listed the two real declarations.
With two identical spellings the "Cast the argument to the parameter type you mean" hint cannot help.

## Repro
`AmbHost.pick(4)` from the cppexp fixture (`Test/library/cpp_interop_explicit.h`, exercised in
`Test/test_cpp_interop.cb` near the `amb_pick` legs); the review's probes and oracles are recorded in
`scratch/briefs/r3_review2.md`.

## Root cause
`spellCandidate` in `LLVMBackend_Overloads.cpp` (ambiguous-tie branch) spells each tied
`FunctionSymbol`'s parameters; a `__cflat_dflt_*` wrapper symbol has only the arity-prefix parameters.

## Fix direction
For a wrapper candidate, spell the full declaration it forwards to: look up
`cxxDefaultWrapperRequests_[c.UniqueName].linkage` and spell the registered full-declaration symbol with
that linkage (show defaulted parameters as `T = ...`, clang-style). Same message text otherwise.
