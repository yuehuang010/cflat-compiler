# Trailing const leftovers after c08 (const plan phase 8)

Filed 2026-10-19 from the c08 implementer report (landed 87b36b2f1). Items 1-3 (#8 field naming,
#10 location for generic interfaces, receiver on internal interface dispatch) fixed in c11.

Design note (not a bug): `T Read() const { return v; }` with T = int* is refused because const is transitive through
pointer fields (phase 2); C++ is shallow. Keep unless the maintainer rules otherwise.

## Left after T471 (Opus r2; master same unless noted)
- LSP: hover/definition trust the resolved-call map by position only (LspServer.cpp ~909/~1252); with a cached index kept for a file
  with errors, stale positions can answer another word. Guard: accept only when the target's short name equals the word under the cursor.
  RegisterResolvedCall is a linear search per call (O(n^2) on large files); key by (file, line, column).
