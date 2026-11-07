# Static locals of C++ class type: leftovers after T93, T226, T233 and T243

Found by T93 review r1 (2026-10-10). Probes: scratch/repro_keep/t93/ (rev_t93_r1.sh drives them; compare
cflat vs clang). T226 fixed the lvalue copy and the non-ternary derived slice. T233 fixed the static
`move` alias, the derived/mixed ternary arms into a base local or static (clang counts, legs 93015-93024).
T233 round 2 fixed the xvalue (`move d`) derived arms, mixed lvalue/xvalue/prvalue arms of one derived
class, `static T g = move *p`, and the moved-source refusal for a second static `move a` (legs
93025-93035). T231 fixed the call-argument double slice `takeb(D(3))`. TERNBASE (af28ab81) fixed the
derived/base lvalue join. TERNMOVE fixed the nested-field arm `rv.mkH2(1).h.d`.

Still open:

1. An exception from a temporary's noexcept(false) destructor inside the static's initializer does
   not propagate: cflat prints `3 3 made=1`, clang `-9 4 made=2` (rev_t93_cleanup.cb).
2. Global C++ class initialized from `move` of another global: cflat refuses any non-constant global
   C++ class initializer ("global variable initializer must be a compile-time constant"), so the
   global move count cannot be compared with clang yet. Not a move bug; revisit with global init design.

## Return of a derived local or parameter (fixed, STATLEFT; legs 93055-93060)

`B mk() { D d = D(6); return d; }` moved nothing: `return d;` of a bare derived local or parameter went
through a copy-only variadic-ctor wrapper (its `sl::D&` parameter copies). C++20 treats it as an xvalue,
so clang moves. The fix requests that wrapper with the returned name as an rvalue first (clang's
copy-init overload resolution on the xvalue) and falls back to the lvalue wrapper (legs 93061-93063:
`B(const D&)` only, `B(const D&)` + `B(D&&)`, explicit `B(B&&)`). Explicit `return move d` with
`B(D&&)` still calls `B(B&&)` (clang `B(D&&)`; master too): the `!explicitMove` guard skips the wrapper. Matrix in scratch/statleft_matrix.md. Parenthesized returns and
reference returns already matched clang and are guarded by legs.

## Still open: array element of a class temporary as a ternary arm

`c ? rv.mkHA(1).a[0] : rv.D(2)` into a plain local base, `B g` (C++ class `B`, element type `D`) gives
`1 2 0 1 3` (clang `1 2 0 2 4`): one move straight from the element, where clang moves the element into a
D temporary and then slices that temporary into B. Reversed arms, `a[1]`, `c ? mkHA(1).a[0] : (c ? ...)`
and `auto` / same-class `D` destinations are the same family. A static local destination already matches
clang (`B1S`, `B2S`). Field-of-temp arms (`mkH(1).d`, `mkH2(1).h.d`) match clang.

Traced (STATLEFT, IR and probes, `scratch/statleft_probe/`):
- The element arm reaches the derived-arm path with `fieldOfTemp=1`: the element's storage is a zero-offset
  GEP into the HA sret temp, so `IsOwnedTempValue` (exact alloca match) is false and
  `IsFieldOfOwnedStructTemp` (GEP walk) is true. The arm is Xvalue and deferred.
- The deferred arm reaches `resolveDerivedArm` with a Prvalue sibling of the same class, and the temp
  branch runs (`EmitCxxCopyOrMoveConstruct` returns success), yet the final IR has no temp move. The
  decl join ends in the generic `cxx_lvalue_storage_join` PHI, and the slice from the PHI moves once.
  Why the temp move is discarded is NOT yet known. Next step: lldb on the decl-join path, not a guess edit.
- Two tried and reverted: (1) recording a built-in-place prvalue sibling as the derived arm state
  (`NoteBuiltInPlaceDerivedArm`): no change in any cell. (2) (not tried) allowing Xvalue/Lvalue x Prvalue
  of one class in `lowerMixedCxxValueJoin` (its same-name gate only admits Lvalue x Xvalue).

Refused on master, clang accepts (not counts; out of this item, do not fix here): `sl.D g = c ? sl.D(2) :
mkHA(1).a[0]` ("cannot initialize C++ class"), `sl.D g = c ? mkH(1).d : sl.D(2)`, and `takeB(c ? mkHA(1).a[0]
: sl.D(2))` (by-value arm ownership error "'?:' arms handle ownership differently").

Same-class NRVO (not a derived-to-base item; master kept): `B b = B(6); return b;` and `D d = D(6);
return d;` move on master, clang elides (0 moves). Same for `DC d; return d;` with a copy-only class.

Not in this item: `B g = PR(D(x));` (prvalue D argument to a by-value D parameter, declaration init)
copies once extra on master and fix (clang 0 copies); `return PR(D(x)).get();` matches clang.
