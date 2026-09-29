# C++ overloads that differ only in const collapse onto one CFlat signature

Found 2026-09-28 (fix/cxx-const-internal, matrix scratch/b1_matrix.md in that worktree). The
pointee-const fix made clang-ranked sets (member templates beside non-templates, template-only
sets, template receivers) agree with clang. Sets CFlat ranks ITSELF still pick the non-const twin.

## Repro (clang++ -std=c++20 value first, CFlat second)

    struct S { long m() { return 11; } long m() const { return 12; } };
    struct K { long plain(S*) { return 1; } long plain(const S*) { return 2; } };
    const S* cpv();  const S& csr();
    long f_kind(S*);  long f_kind(const S*);          // free twins, 1 / 2
    template<class A> long f_tkind(A); long f_tkind(S*); long f_tkind(const S*);

- `k.plain(cpv())` 2 / 1, `b1.f_kind(cpv())` 2 / 1, `b1.f_tkind(cpv())` 2 / 1
- `cpv()->m()` 12 / 11, `csr().m()` 12 / 11 (const T& receiver, no pointer involved)
- C++ field `const S* cp; k.kind(h.cp)` 2 / 1 (a field is not a call result: no pointee flag)

## Root cause

Member registration dedups by CFlat signature key and keeps the non-const twin
(LLVMBackend_CInterop.cpp, "Prefer a non-const method over its const twin"); free twins keep the
first registered (`--symbol b1.f_kind` lists one overload). CFlat's own ranking never sees the
const twin. A free template beside the twins is ranked by CFlat against the surviving twin.

## Fix direction

Keep both twins bound (distinct unique names) and break the tie in CFlat overload resolution with
TypeAndValue::IsCxxPointeeConst (argument) vs the parameter's pointee const, and IsCxxConstRef /
IsCxxPointeeConst of the receiver vs the method's `const`. Or route such sets through clang like
member templates. Mark parameter pointee const in MapRawSig and the member mapper (cache round-trip
already exists: "cpc"). C++ fields: set IsCxxPointeeConst on the field TypeAndValue (StructData
serializer). RequestCxxFreeFunction / RequestCxxBraceFunction spell pointer arguments without
pointee const too (no observable std overload found).
