# Bitfield inside an anonymous union unreachable; enum-typed bitfield fails the import

Pre-existing on master (found 2026-09-27 by the fix/nua-bitfield review):
- A bitfield member of an anonymous union: `c.__anon0.a` -> "Unknown identifier 'a'" (plain `c.a` too).
  Repro scratch/revAF_c/q_S15v.cb.
- A bitfield of enum type (`enum E e : 3;`) makes the whole header import fail with "unsupported
  underlying type".
Fix direction: flatten anonymous-union bitfields like other anonymous members; map an enum bitfield to
its underlying integer storage with the enum as the field type.

## Update (fix/c-bitfield review, 2026-09-27) - bumped to p2 (wrong values)
- C enum bitfield read with the wrong signedness: `enum AHE en:2` holding 2 reads as -2 in CFlat (clang: 2).
  LLVMBackend_VariablesAndIR.cpp ~1124 treats an imported enum bitfield as signed unless IsUnsigned is set; an
  enum with no negative enumerators is unsigned-backed (clang: underlying type unsigned int).
- Union bitfield members (named union too, not only anonymous): `union AHUnion { ...; unsigned long long b:40; }`
  -> `u.b` is "Unknown identifier 'b'". LLVMBackend_CInterop.cpp ~11928 skips union field verification.
Probes: scratch/revAH_c/ (main checkout).
