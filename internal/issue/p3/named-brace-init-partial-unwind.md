# Named brace init: `{ h = move *p }` refused

Out of scope of T261 (partial-unwind fix landed). A named field initialized with a move of a
dereferenced raw pointer is refused "dereference of moved variable 'p'" on master and after the fix;
clang moves from `*p` (probe case c01 in the T258 review repro set).

## Left after T430 (review 1, pre-existing on master)
- Field REPLACEMENT window: while a named override replaces an already-built field (EmitOneFieldInit,
  MainListener_Expressions.cpp ~24305-24346, tracking paused ~25929), a throwing copy/converting ctor of the new
  value leaks the object's other fields (`PC w = { t = c };` with tc_throw: live=1). Ideal live=0, dtors=2. Fix
  direction: during the pause register per-field unwind entries for every field except the one being replaced.
- Call argument: a completed brace argument leaks when a LATER argument throws (`take2(w: { z = 1 }, y: t261.bval(1))`,
  live=1); its unwind entry is released when EmitConstructedFieldOverrides returns.
