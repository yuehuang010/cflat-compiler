# Union holding a C++ class field: brace init `{ m = x }` constructs nothing

**RULED 2026-10-21:** Refuse non-trivial C++ class members in CFlat unions for now - an NYI refusal, not a hard no. See fix-issue-lessons.md digest.

Summary: `U u = { m = x };` where m is a C++ class member of a CFlat union runs no constructor
(count 0); clang++ -std=c++20 copy-constructs m from x. The member is then used unconstructed.

Found: T117 review 1 (2026-10-11), pre-existing on master. Probe header: T117 worktree
scratch/rev_t117_rv.h.

Fix direction: decide whether unions may hold non-trivial C++ classes at all (C++ requires the
user to manage the active member; a refusal may be the CFlat-correct answer) - needs a ruling; if
allowed, the named member of a union brace init must be constructed in place.

## 2026-10-15: no ruling yet
Maintainer is researching union semantics (C++: non-trivial member deletes the union's special members). Do not start
either direction (refuse vs construct in place) until ruled.
