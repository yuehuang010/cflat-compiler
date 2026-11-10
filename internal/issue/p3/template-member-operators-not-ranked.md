# Protected template member operator is not refused

Summary: a protected `template<class T> operator+(T)` beside a free `operator+(const PT&, double)`: `t + 2` picks the
free candidate. clang selects the protected member and rejects the access. Public member templates now rank with free
operators except for compound and rewritten operators (box 2026-11-09).
Repro: `cppsopd.T456ProtectedTemplateFree value = default; return value + 2;` (Test/library/cpp_interop_strop.h).
Attempts 2026-11-09 (Luna, 3): the check by exact template owner, by class `memberAccess`, and by forced record projection
all missed it. Routing every set with a same-named member template through clang restores the refusal but breaks unrelated
sets (test_cpp_interop rc=5, `le >> 1` routed wrongly), so that broad fallback was removed.
Found: T456 r2 opus review, 2026-10-24 box, PRE-EXISTING

Compound and rewritten operators, a member template beside a free candidate (same on master, box 2026-11-09):
- A non-viable member template is a hard error: `x += 2` (clang 61), `x < 1` with a `<=>` template (clang picks the free, 12).
- A viable template beside an exact non-template free takes the template: `x < 1` gives 11, clang 22.
- A viable template with an ill-formed body that clang does not select is refused (clang 61 and 2).
Skipping the member-template request when a free candidate exists fixes the first and breaks viable templates and
selected-body errors (11 becomes 22, invalid programs accepted). Only clang ranking the joint set can do both.
