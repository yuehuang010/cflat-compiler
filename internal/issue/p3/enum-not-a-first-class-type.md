# Enums are half first-class: no type entry, int32-only initializer, wrong signedness

Consolidated 2026-09-03 from four files filed during the enum case-label work (q03) and the
promotion review (c56efdf): `enum-not-usable-as-parameter-or-return-type`,
`namespaced-enum-unreachable-qualified-and-no-forward-ref`,
`enum-initializer-outside-int32-aborts-compiler`, `enum-narrow-base-widens-with-sign-extension`.
One root cause, four visible legs. Identical on master 94ec41c and later.

## Summary

`ParseEnumSpecifier` (cflat/MainListener_Declarations.cpp ~3280) registers each member as a
constant global in `constFoldableGlobals_` and nothing else: no type entry for the enum NAME, no
ForwardRefScanner pass, `std::stoi` on the initializer text, and the member's `TypeAndValue`
does not carry the declared base's signedness. Consequences:

1. **Not a parameter or return type.** `Dir d = Dir.Fwd;` works as a local, but
   `int classify(Dir d)` fails with `cannot find the type 'Dir'` (probe scratch/q03e_param.cb).
   The local path resolves the enum to its backing type; `ParseDeclarationSpecifiers` (both
   copies) does not find it in the struct / alias / builtin tables.
2. **No qualified access, no forward reference.** `namespace Ns { enum Dir : int { Back = -1 }; }`
   then `Ns.Dir.Back` from outside fails with "'Dir' is not a member of namespace 'Ns'"
   (unqualified inside the namespace works). A member used before the enum's declaration fails
   with "Undefined variable Later." - enums are registered by the main pass only.
3. **Initializer range is int32 regardless of base.** `enum E : i64 { Min = -3000000000 };`
   terminates the compiler with an uncaught `std::out_of_range` from `std::stoi`. Positive values
   past INT32_MAX on i64/u64 take the same path.
4. **Narrow unsigned base sign-extends.** `enum Small : u8 { B = 200 }; Small s = Small.B;
   return (int)s;` yields -56; clang gives 200. Any enumerator above 127 reads negative once
   widened (probe scratch/q03m1_rev2_p3.cb).

## Fix direction

One change in the declaration pass, mirrored where the fact is consumed:

- Pre-register the enum in `ForwardRefScanner` (name -> backing type, members -> constants), so
  forward references and the main pass see the same table. Register the NAME as a type alias to
  its backing type under the scoped key (same mechanism as `using Dir = int;`), so both
  `ParseDeclarationSpecifiers` copies resolve it for parameters, returns, fields; make sure
  `ResolveFuncPtrTypeSpelling` still collapses it for signature comparison.
- Route member lookup for a dotted spelling through the scoped helpers landed in 94ec41c
  (`FindFirstVisibleScoped` over the enum registry) so `Ns.Dir.Back` resolves.
- Fold the initializer through `ParseNumberConstant` / the compile-time folder into an APInt of
  the base's width and signedness; `LogErrorContext` "enum value '{}' does not fit the backing
  type '{}'" when it does not fit (catalog-visible form).
- The member's `TypeAndValue.TypeName` is the base type spelling, so `IsUnsignedInteger` and
  the sext/zext choice in `Upconvert` / `CreateCast` (LLVMBackend_VariablesAndIR.cpp) follow the
  base. Check whether a new TypeAndValue/StructData field is needed; if so it goes into the
  LLVMBackend.cpp cache round-trip in the same change (`--init` serializer rule).

Also unblocks the enum-member leaf of `p3/if-const-scan-folder-misses-sizeof-enum-cast` (the
folder can consult the pre-registered table) - fix that leaf there, not here.

## Legs

Test/test_c.cb next to the switch enum legs: `int classify(Dir d)`, `Dir make()`, a switch on a
qualified `Ns.Dir.Back` label. Test/test_basic.cb enum coverage: namespaced enum qualified from
outside, enum used before its declaration, `: u8` with 200, `: u16` with 40000, `: i8` with -1
widened to int, comparison `s > 100`, i64 enum below INT32_MIN and above INT32_MAX, u64 with 2^63.
Test/errors/err_switch_case_range.cb (or a new err_enum_range.cb if the message is new): u8 enum
member of 256. Afterwards `--symbol` on a `Dir` parameter on macOS; `test_lsp.bat` on Windows.
