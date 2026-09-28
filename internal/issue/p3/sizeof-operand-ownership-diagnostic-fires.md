# Ownership diagnostic fires inside an unevaluated sizeof operand (ruling needed)

After fix/sizeof-eval (operands unevaluated, IR erased), `sizeof(helper(n))` where `helper` stores
`n` into a borrowing list still reports "still owns the object it was given"
(owningLocalBorrowingHelperArgs_, LLVMBackend.h ~2837, appended LLVMBackend_Overloads.cpp ~5543 -
not rewound by EvaluateOperandTypeOnly). Nothing is stored at run time. Found 2026-09-27 by the
fix/sizeof-eval round-3 review (probe scratch/rev3/rev3_szo_helper.cb in that worktree, now in the
main checkout scratch/ if copied).

## Question for the maintainer

Should ownership / flow diagnostics fire for code inside an unevaluated operand? C++ still type-checks
an unevaluated operand (ill-formed code is an error) but has no ownership analysis. Proposed: type
errors fire, ownership/flow diagnostics do not (rewind this ledger like the others).
