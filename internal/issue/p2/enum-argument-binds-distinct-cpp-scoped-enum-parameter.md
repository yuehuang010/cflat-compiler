# A signed unscoped enum (C++ or CFlat) argument binds a C++ scoped-enum parameter of a different type

clang refuses passing an enumerator of one enum to a parameter of a different scoped enum. T40 refuses it for an
UNSIGNED unscoped C++ source, but a SIGNED one (`enum Signed : int { SignedVal = -3 }`, or an implicitly backed
`enum Neg { N = -6 }`) and a CFlat `enum Own : int { V = 6 }` still bind: `t40.only(t40.Signed.SignedVal)` compiles
and runs with -3; `onlyref(const S&)` too; `choose(S)` (11) wins over `choose(int)` (clang 10). Master is the same.
Found by the T40 Sol final review (2026-10-05). Probes: scratch/repro_keep/t40_sol3/finding/ (h.h + .cb + C++ twin).

## Root cause (Sol)

CxxDistinctEnumArgument (LLVMBackend_StateAndImports.cpp ~944, called at LLVMBackend_Overloads.cpp ~1474) reads
only the lowered arg.TypeName. Call lowering keeps the signed/unscoped source identity in
NamedVariable.InferSourceTypeName (MainListener_PostfixExpression.cpp ~7161) and copies only scoped/unsigned
identities back to TypeAndValue; the adjacent integer-refusal helper already consults InferSourceTypeName.

## Fix direction

Prove distinct-enum from the preserved source enum identity. Related: unscoped U1 -> unscoped U2 is still accepted
(clang refuses), pinned by read-only leg Test/test_cpp_interop.cb ~1627 - needs a ruling before that leg changes.
