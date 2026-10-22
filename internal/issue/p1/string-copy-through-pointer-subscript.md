# p1: string copy through a pointer subscript still shallow-adopts (double free)

Leftover from T356 (which fixed `string t = *ps;`, `string t = ps[i];` declarations, `h.value = *ps`, `*(ps + i)`).

## Repro (ASan double-free, master)
```cflat
string* gq;                                   // 1. global pointer subscript
int f() { string t = gq[0]; return 0; }
int g(string* ps, H* h) {
    h.value = ps[0];                          // 2. field store of a subscript
    string t = "x".copy(); t = ps[1];         // 3. plain assignment of a subscript
    string u = (ps)[0];                       // 4. parenthesized base, also (*pp)[0]
    return 0;
}
```
Reviewer cases: cflat-fix-t356-derefstrcopy scratch/rev_t356_opus_tmp/ c47, c57/d66, c62, c50/c63 (kept in
scratch/repro_keep/box1021/t356_rev/).

## Root cause
- T356 gate (MainListener_Declarations.cpp, isNamedPointerSlot / directSubscript) covers declarations only and
  needs an Identifier primary bound in stackNamedVariable: FindVariableByStorage (LLVMBackend_MoveDataflow.cpp)
  never sees globals.
- Field store: MainListener_Expressions.cpp srcIsDerefLvalue rejects the GEP, no subscript gate.
- Plain assignment: ParseAssignmentExpression local string copy path has no deref/subscript gate.

## Fix direction
Share the declared-pointer-subscript gate as one helper used by declaration, assignment and field-store
paths; resolve globals through globalNamedVariable; unwrap parenthesized primaries. Controls that must keep
the old path: raw-new arrays, views, fixed arrays, lists, `o->ps[0]`.
