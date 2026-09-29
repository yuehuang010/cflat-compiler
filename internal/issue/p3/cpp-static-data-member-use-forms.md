# C++ static data members: chained field access and instance-receiver access refused

Found 2026-09-28 by fix/inherited-members (pre-existing on master; not inheritance-specific).
clang++ accepts both; CFlat refuses at compile time (no miscompile).

- Field of a class-type static member, spelled through the type: `sd.ClsLib.origin.y` /
  `sd.ClsLib.origin.x = 3;` -> "'y' is not a member of namespace 'sd.ClsLib.origin'". The
  parenthesized `(sd.ClsLib.origin).y` and `sd.P o = sd.ClsLib.origin;` work. The qualified-name
  walk in MainListener_PostfixExpression.cpp breaks out at the data-structure qualifier and never
  treats the bound static global as an object for the next `.`.
- Static data member through an instance: `sd.OolInline d = default; d.K` -> "Unknown identifier
  'K'" (C++ allows `obj.K` for a static member).
Repro header: `namespace sd { struct P { int x = 0; int y = 0; }; struct ClsLib { inline static P
origin{8, 9}; }; struct OolInline { static int K; }; inline int OolInline::K = 17; }`.
