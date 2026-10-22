# [immovable] CFlat struct: no factory-function return

**RULED 2026-10-21:** Allow by-value return of fresh operands (brace-init, default, same-type call, ternary of those) of an [immovable] CFlat struct, built in the caller's sret slot; named locals stay refused. See fix-issue-lessons.md digest.

Summary: T90 (2026-10-10) refuses every by-value return of an [immovable] CFlat struct, including a
fresh `return { x = 1 };` / `return default;`. C++ [immovable] classes are accepted for fresh operands
(C++17 guaranteed elision into the sret slot). CFlat structs have no guaranteed-elision contract yet,
so factory functions for immovable CFlat types are impossible.

NEEDS MAINTAINER RULING: should a fresh operand (brace-init, `default`, a call returning the same
immovable type, ternary of those) be returned by value with guaranteed in-place construction, like
C++17? Proposal: yes, same freshness predicate as the C++ path (IsImmovableFreshOperand), lowered
through the existing sret slot.
