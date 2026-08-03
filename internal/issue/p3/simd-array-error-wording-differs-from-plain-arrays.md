# simd<T,N>[N] gets DIFFERENT wording than a plain array for the identical error

**Severity: P3 - wording only. No miscompile, no false accept, no false reject.** Both shapes are
rejected; they are rejected by DIFFERENT guards, so the user sees two different messages for what
is the same language limitation.

## Summary

The two guards that produce this wording in `MainListener.h` (there are seven `!IsSimd` sites in that
file; only these two are in play here) carry a `!IsSimd` exclusion that was harmless while the
simd branch never recorded an array dimension. `fix/simdptr` (`RecordSimdPointerAndDims`) makes
`ConstArraySize` non-null for `simd<T,N>[N]`, so those exclusions are now REACHABLE and actively
route the simd shape past the plain-array message. A second, later guard catches the shape anyway,
so nothing is accepted that should not be - only the wording (and the suggested remedy) differs.

## The two sites

- `cflat/MainListener.h:12447` - whole-fixed-array ASSIGNMENT rejection:
  `if (namedVar.TypeAndValue.ConstArraySize > 0 && !...IsArrayView && !...IsSimd && ...)`.
  The simd shape falls through to the MULTI-DIM ROW guard a hundred lines further down the same
  lambda, `cflat/MainListener.h:12560-12571`, which is what actually emits the second message
  below. That guard calls the static helper `DescribeAggregateStorageShape`
  (`cflat/LLVMBackend.h:14918`) for the shape text. It is NOT `CreateCast`'s aggregate-store guard
  (`cflat/LLVMBackend.h:15020-15043`, reached from `CreateAssignment`), whose wording is different
  ("cannot store a pointer value into ..." / "cannot store a single scalar value into ...").
- `cflat/MainListener.h:10090` - GLOBAL fixed-array initializer rejection:
  `if (!externDeclOnly && typeAndValue.ConstArraySize > 0 && !...IsArrayView && !...IsSimd && ...)`.
  The simd shape falls through into `CreateGlobalVariable`, whose wide-storage/single-value guard
  at `cflat/LLVMBackend.h:14356-14365` is the actual emitter of the pointer/string-literal message
  below.

## Measured repro (x64/Release/cflat at fix/simdptr, macOS arm64)

Whole-array assignment - `scratch/r3_item5_plain_assign.cb` vs `scratch/r3_item5_simd_assign.cb`:

```
float[2] fa = default; float[2] fb = default; fa = fb;
  -> assignment to a whole fixed array 'float[2]' is not supported - assign its elements
     ('fa[i] = ...'), or copy at the DECLARATION instead ('float[2] dst = <source>;'), which is
     supported.

simd<float,4>[2] sa = default; simd<float,4>[2] sb = default; sa = sb;
  -> cannot assign to 'sa' as a whole - it names fixed array 'simd<float,4>[2]', and CFlat has no
     whole-array assignment. Copy the elements ('sa[i] = ...'), or bind an array view
     ('T[] row = sa;') and write through that.
```

Global initializer - `scratch/r3_item5_plain_global.cb` vs `scratch/r3_item5_simd_global.cb`:

```
float[2] src = default; float[2] g = src;
  -> cannot initialize global fixed array 'float[2] g' from this expression - a global's
     initializer must be a compile-time constant of the array's own shape. ...

simd<float,4>[2] ssrc = default; simd<float,4>[2] gs = ssrc;
  -> cannot initialize global fixed array 'simd<float,4>[2]' 'gs' from a pointer value or a string
     literal - CFlat has no C-style character-array initializer. ...
```

Both exit 1 in every case. The second global message is also factually off for this input: there is
no string literal and no pointer in the source, which is the cost of the shape landing on the
string-literal sibling instead of the "must be a compile-time constant" one.

## Fix direction

Align the wording by dropping the `!IsSimd` exclusion at both sites so the simd shape reaches the
same message every other element type reaches. `DescribeArrayShape` already renders the vector
spelling (`simd<float,4>[2]`), so the messages come out correct without further work.

**Do NOT do this without accept-set discipline.** Removing `!IsSimd` WIDENS a rejection predicate,
and false rejections are the most expensive failure mode in this repo (see
`internal/fix-issue-lessons.md`, "On guard polarity"). Before touching either guard, enumerate and
freeze as value legs every simd shape in the neighbourhood that currently compiles and runs - bare
`simd<T,N>` assignment and decl-init, `simd<T,N>*`, `simd<T,N>*[N]`, `simd<T,N>[N]` element
assignment, a global bare `simd<T,N>`, a global `simd<T,N>[N]` with `= default` and with a brace
list - and only then widen. The benefit is wording alone, so the bar for regressing an accepted
program is zero.

Deliberately left alone by `fix/simdptr` for exactly that reason.

## Related, same family, also wording only

`DescribeArrayShape`'s SECOND overload (`MainListener.h`, the one taking loose
`typeName/stars/n/innerDims`) renders the INITIALIZER side and has no simd information to work
with, because the decl-init path infers the source as a bare type name. So a mismatched copy prints
one side with the vector spelling and one without:

```
simd<float,4>[3] src = default; simd<float,4>[2] dst = src;
  -> cannot initialize fixed array 'simd<float,4>[2]' from 'float[3]' - a fixed-array copy
     requires identical element type and extents
```

Closing this needs a `srcIsSimd`/`srcLanes` pair threaded alongside `srcInferredTypeName` through
the decl-init inference, which is a wider change than the two guards above.
