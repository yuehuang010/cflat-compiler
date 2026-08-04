# A `simd<T,N>*` pointer declaration aborts the compiler

Filed 2026-08-02 while fixing [[empty-brace-initializer-never-seeds-and-crashes-on-defaults]].
PRE-EXISTING and unrelated to that fix: measured identical on `5a6580c` and on the `fix/emptybrace`
commit. Found because the empty-brace ruling's diagnostic suggests `nullptr` as the unambiguous
remedy, and for this one type the suggested remedy crashes.

Severity: compiler abort (rc 138, zero output), no diagnostic.

## Repro - measured with `-o` on Release builds of `5a6580c` AND `fix/emptybrace`

BOTH initializer spellings abort, not just `nullptr`:

```cflat
extern int main(){ simd<float,4>* sp = nullptr; printf("sp=%lld\n", (i64)sp); return 0; }
extern int main(){ simd<float,4>* sp = default; printf("sp=%lld\n", (i64)sp); return 0; }
```

```
compile rc = 138, no output, no diagnostic     // both spellings, both binaries
```

The `= default` face was found by mutation-testing the empty-brace reject leg, AFTER the first
draft of this file claimed only `nullptr` aborted. That claim was written from one measurement and
was incomplete - recorded here rather than silently corrected, since the difference matters: it
means BOTH remedies the empty-brace diagnostic names are broken for this one type.

The bare declaration is a clean (if oddly-worded) diagnostic on both binaries, so the abort is
specific to the initialized form:

```cflat
simd<float,4>* sp;
// simd<T,N> supports the static calls '.load(array, index)' and '.store(vector, array, index)'.
```

The empty-brace spelling `simd<float,4>* sp = {};` USED to reach the same abort by a different
route - the seeding arm stored a whole VECTOR into the 8-byte pointer slot. `fix/emptybrace`
rejects that spelling outright (`Test/errors/err_ptr_brace_init.cb` pins it), so `= {}` is now a
located diagnostic. Neither `= nullptr` nor `= default` is, and that is what is left open here:
`simd<T,N>*` currently has NO initialized declaration spelling that compiles.

## Root cause - NOT diagnosed

Not investigated. `ParseDeclarationSpecifiers` sets `declType.Pointer` from `declSpec->pointer()`
on the simd branch (`MainListener.h` ~3816), so a `simd*` type flows on with both `IsSimd` and
`Pointer` set, and something downstream almost certainly asks `GetType` for the vector rather than
the pointer. That is a citation, not a measurement - read the abort's stack before trusting it.

## Fix direction

Decide first whether `simd<T,N>*` is a supported type at all. If it is not, the bare-declaration
message above is the right diagnostic and it simply needs to fire for the initialized forms too.
If it is, the pointer-ness has to win over `IsSimd` wherever the slot type is computed. Per
CLAUDE.md, an LLVM/compiler abort gets a proper error message either way.

## PARKED 2026-08-03 - branch `fix/simdptr`, worktree `../cflat-fix-simdptr`

Two review rounds, stopped before a third at the maintainer's instruction. The branch is ONE
commit `308a4e3` on `904f026`; nothing merged, `master` untouched. The abort itself is FIXED and
the approach is confirmed right - what is left is documentation-correctness and two sibling
diagnostic sites, not a wrong design.

**Both premises in the section above are WRONG, measured. Do not act on them.**

- **`simd<T,N>*` IS supported.** On a `904f026` build a `simd<float,4>*` parameter with `&v`
  compiles and runs, a deref returns correct lanes, and global / struct-field / union-field
  pointers all compile. `GetType` (`LLVMBackend.h:17114`) has carried an explicit
  `simd<float,8>* lowers to <8 x float>*` arm since the type landed. ONLY the local declaration
  was broken. Rejecting the type would have been a false rejection of working code.
- **The "bare declaration" above is not a declaration diagnostic at all.** Inside a block,
  `simd<float,4>* sp;` parses as the EXPRESSION `simd<float,4> * sp`, and the message comes from
  `ParseSimdStaticMethod` (`MainListener.h:19258`) via a `postfixExpression`. Using it as the
  oracle would have pinned a mis-parse. Confirmed independently by the reviewer.
- **The root cause guess was half right.** All five crashing spellings land on
  `LLVMBackend::SplatToSimd`, `cast<FixedVectorType>(GetType(tv))`, from
  `MainListener::ParseDeclaration`. `GetType` is NOT the defect - its `allowPointer` defaults to
  true and it correctly returns the pointer. The defect is the unchecked `cast` of that pointer,
  plus a missing `!Pointer` condition on the splat arm.

**What the branch already does, reviewed and cleared:** gates the decl-init splat on the slot
actually lowering to a `FixedVectorType`; adds a `dyn_cast` backstop in `SplatToSimd`; records
pointer depth AND array dimensions on simd declared types via one shared
`RecordSimdPointerAndDims` called from both `ParseDeclarationSpecifiers` copies; splats a scalar
into vector storage in `CreateAssignment`. Bar green: `test.sh` 576/0/8, `example_mac.sh` 35/0,
`test_lsp.sh` 152/0, `test_hpc.cb` 282/282, differential `--check` sweep over 523 files = 3 real
diffs (the three touched test files).

**Cleared and NOT to be re-litigated:** removing the `!returnType.IsSimd` carve-out at
`MainListener.h:8519` is safe (the `AliasArraySize` half is unreachable - no `using` alias can
spell a simd array at all); the `sizeof` 16 -> 32 layout change for a `simd<T,N>[N]` struct field
and global is correct and has zero in-tree uses; `CreateAssignment`'s blast radius is bounded to
three `FixedVectorType` construction sites; the `0741952` multi-dim rejection still fires;
`--init` cold-vs-warm is identical with all four fields round-tripping in both serializers.

**The four open findings:**

1. **The recorded MECHANISM of the pre-fix miscompile is false, in three places.** The bad splat
   wrote every EVEN lane, not lane 0 only: measured `f4 5 0 5 0`, `f8 5 0 5 0 5 0 5 0`,
   `i4 7 0 7 0`. The comment at `Test/test_hpc.cb:797`, the source comment at
   `LLVMBackend.h:14527`, and the landed record all say "lane 0 only". That is load-bearing, not
   cosmetic: it tells a future author that any lane except 0 is a safe discriminator, and lane 2
   is exactly as vacuous as lane 0. The chosen `r[3]` leg discriminates only because 3 is odd; the
   all-lane sum is the robust one.
2. **Two sibling describe helpers still drop the simd spelling.**
   `LLVMBackend.h:10153` (`DescribePointerShapedInterfaceSource`) and `MainListener.h:17486`
   (`DescribeArrayShape`) both test `ConstArraySize != 0` BEFORE `IsSimd`, and both were written
   when a simd type could never carry a dimension. Now reachable, they name the bare lane type -
   `'float[2]'` for a `simd<float,4>[2]`, `'float*[2]'` for `simd<float,4>*[2]` - which is the
   exact wrongness this same commit taught `DescribeAggregateStorageShape` to avoid. Diagnostic
   only, but the fix is inconsistent: one helper learned the spelling, two did not.
3. **`srcIsUnsigned` was threaded into 1 of 3 splat sites.** `LLVMBackend.h:14528` passes it;
   `15464` (`CreateVectorOperation`) and `15530` (`SplatToSimd`, decl-init) still take the
   default. With `u32 x = 4000000000`: `a = x` gives `4000000000` but `simd<i64,2> b = x` and
   `c = z + x` both give `-294967296`. Previously both were consistently wrong; fixing one leg
   makes it a live inconsistency and falsifies three statements shipped in the same commit
   (`LLVMBackend.h:14524` "the same rule the declaration initializer uses", `doc/HPC.md:165`
   "splats the same way", and the record's "at a declaration and at an assignment alike").
   Thread it at all three sites or revert it.
4. **`Test/errors/err_fixed_array_byval_return.cb:48` asserts nothing**:
   `return v[0] == 1.0 ? 0 : 0;` - both arms are `0`. Should be `? 0 : 1`.

Plus a latent note: `MainListener.h:12451` and `:10094` still carry `!IsSimd` exclusions that are
now reachable for `simd<T,N>[N]` and survive only because a second guard catches the shape. No
miscompile - simd arrays just get different WORDING than `float` arrays for the identical error.
Worth aligning while the same-class audit is open.

**The recurring lesson from both rounds, already added to the landed record:** when a change makes
a field newly non-null, re-audit every guard that reads that field under the NEW conditions. Both
rounds' top finding was a guard correctly classified against master's conditions and wrong against
the commit's.
