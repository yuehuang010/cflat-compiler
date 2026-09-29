# C++ ctor ranking: leftovers after C5

Summary: from the C5 review (2026-09-29). C5 ranks constructors per argument by conversion category and refuses ties the way clang does.
- The declaration form skips the new ranking for literal arguments:
  - `ra.B2 x = ra.B2(1)` with `B2(int, int k=g())` beside `B2(long)`: clang 5, cflat 2. The expression form `ra.B2(1).chosen` is fixed and gives 5.
  - `ra.B3 x = ra.B3(1.5)` with `B3(double)` beside `B3(float)`: clang 1, cflat 2. The declaration path retypes the double literal to float before SelectCxxConstructor sees it.
- `new ra.B2(a)`, where the survivor has a non-constant default argument, is now refused ("default argument that is not a constant"). clang gives 5; master silently picked 2. This refuses valid C++, but it replaces a wrong pick. Fix: route `new T(args)` with a blocked survivor to clang, as the declaration path does.
Probes: the C5 worktree scratch/c5/ (ra.h, ra3.h), copied to scratch/repro_keep/c5/.
