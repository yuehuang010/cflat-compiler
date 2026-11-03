# simd<T,N> alignment with C++26 std::simd

Status: OPEN - maintainer reviewing each part (2026-10-31). Nothing ratified, nothing built, no p4 files created.
Each row's "Direction" is a proposal until the maintainer confirms it; confirmed rows get a RULED line here.

## Plan

Refactor CFlat's `simd<T,N>` so its semantics follow C++26 `std::simd` ([simd], P1928: `basic_vec` / `vec<T,N>`,
`basic_mask` / `mask<T,N>`) wherever CFlat has no safety reason to differ, while KEEPING CFlat's syntax sugar: the
`simd<T,N>` / `simd<bool,N>` spelling family, static methods `simd<T,N>.load/store/select/reduce_*/lanes()`, scalar
splat in arithmetic, postfix-only `++`/`--` (ruled lane-wise 2026-10-31), the gather/scatter extras, and the deliberate
exclusions (no transcendentals, power-of-two N). The comparison below is the inventory; the gap list in section 7 is
the candidate work, to be ratified row by row before any build. Bugs the comparison surfaced (`!v`/`~v` on float lanes
hit the LLVM verifier; float/double/i32 simd by-value parameters lose the vector type) are compiler defects, filed
separately when the maintainer says so, not plan items.

## Comparison (2026-10-31)

Source notes: C++26 side is the working draft [simd]; CFlat side is `doc/HPC.md` (simd section),
`cflat/LLVMBackend_VariablesAndIR.cpp` (`CreateVectorOperation`), `cflat/MainListener_Expressions.cpp`
(`ParseSimdStaticMethod`), `Test/test_hpc.cb`, `Test/errors/err_simd_*.cb`. Every CFlat claim marked "(probed)" was
compiled with `x64/Release/cflat <file> --run` on master a3ff4fac; probes lived in `scratch/simd_cmp/` (deleted).
"keep" in the Direction column = CFlat's current shape is the right one for CFlat.

## 1. Type, width, element types

| Topic | C++26 | CFlat | Gap / divergence | Direction |
|---|---|---|---|---|
| Spelling | `vec<T, N>` (`basic_vec<T, Abi>` with an implementation-defined ABI tag) | `simd<T, N>`, a builtin primitive, no ABI tag | Name differs; CFlat exposes no ABI tag | keep `simd<T,N>` (a flattening may rename; no safety reason) |
| Deduced width | `vec<T>` = native width for T on the target; `vec<T,N>::size()` static | N is mandatory; no `simd<T>`; no `.size()` / `.lanes` constant (only `lanes()` iota) | Gap: no native-width type, no size query | add `simd<T>` (native width) only if a target-portable kernel story is wanted; cheap: a `sizeof`-style `lanesof(simd<T,N>)` or make `N` queryable |
| Lane count | Any 1 <= N <= implementation max (at least 64); non-power-of-two OK | Power of two in [2, 64]; N may be any constant-folded expression (`const int L`, `2*2` probed) | Divergence: N=1, 3, 6, 12 refused with a hint | keep (deliberate hardware-mapping rule, documented) |
| Element types | Every arithmetic type except bool (incl. extended FP types); mask element is separate | `i8`-`i64`, `u8`-`u64`, `float`, `double`; `bool` only as the mask spelling `simd<bool,N>` | Same set in practice; CFlat reuses the vec spelling for the mask | keep |
| Alignment query | `alignment_v<V>`, `flag_aligned` loads | `sizeof(simd<float,4>)` = 16 (probed); no alignment query | Gap, minor | add `alignof(simd<T,N>)` if it does not already fold; no flag needed |
| Pointers / arrays of vectors | Ordinary class type: `vec<T,N>*`, arrays, members all fine | `simd<T,N>[2]`, struct field, global (`= default`) OK (probed); `simd<T,N>*` REFUSED ("a pointer to a simd vector is not supported", probed) | Divergence; `doc/HPC.md` still says `simd<float,4>* p = &v;` works - doc is stale | ruling needed: allow the pointer (C++ parity) or keep the refusal and fix the doc |
| By-value parameter | Ordinary | `simd<u32,4>` parameter works (`test_hpc.cb` t84); `simd<float|double|i32|int,N>` parameters register as the SCALAR type, so every call fails "no overload matches" (probed, 6 shapes) | BUG, not a design gap | fix (p2-grade: the documented value type cannot be passed to a function) |
| Conversion to scalar | None | None ("cannot convert ... to a single value") | same | keep |

## 2. Construction and conversion

| Topic | C++26 | CFlat | Gap / divergence | Direction |
|---|---|---|---|---|
| Default | Indeterminate values | `simd<float,4> v;` reads 0.0 (probed); `= default` is the documented spelling | CFlat zero-fills (safer) | keep |
| Broadcast | Implicit ctor from a scalar `U`, constrained to a value-preserving U -> T conversion (or a non-arithmetic U, or a constexpr wrapper whose value fits) | Scalar initializer, scalar assignment, scalar operand all splat (`v = 3.0;`, `10.0 - a`, probed) | Same ergonomics; constraint differs (see next rows) | keep splat everywhere |
| Broadcast, float -> int | Ill-formed | Refused: "implicit conversion from 'float' to 'int' loses the fractional part" (probed, init and operand) | agree | keep |
| Broadcast, int -> float, float -> double | int -> float is NOT value-preserving in C++26 (so `vec<float> + 1` needs `1.0f` or `std::cw<1>`); float -> double OK | `v + 2` with float lanes converts silently; `double vec + float scalar` OK (probed) | Divergence: CFlat is looser on int -> float | keep (CFlat's int -> float promotion is a scalar-wide rule) |
| Broadcast, narrowing int | Ill-formed (`vec<int8_t> + 300`, `vec<uint32_t> + -1`) | `simd<i8,4> v = 300;` -> 44, `v + big` (300) -> 45, `simd<u32,4> v + -1` wraps (probed). SCALAR CFlat does the same (`i8 x = 300;` -> 44, probed), so this is CFlat-wide, not simd-specific | Silent divergence from C++26 (and from the 2026-09-04 no-implicit-narrowing-at-calls ruling, which covers calls only) | leave to the scalar narrowing ruling; simd should inherit whatever scalars get |
| `simd<float> + 1.0` | Ill-formed in C++26 (double literal into float lanes) | Accepted: `1.0` is float in CFlat (deferred ruling 2026-10-13) | Not a divergence in CFlat's own type system | keep |
| Converting between element types, same N | `basic_vec(const basic_vec<U,A>&)`: implicit if value-preserving, `explicit` otherwise - so `vec<double,4> d = f;` and `vec<int,4>(f)` both work | Refused, even with a cast: "a simd vector converts only to the same lane count and element type ... rebuild the value" (`simd<double,4> d = f;` and `(simd<double,4>)f` both fail, probed) | GAP: no widening/narrowing of lanes at all; the only route is store, scalar loop, load (that is what t84 in `test_hpc.cb` does) | p4: implicit value-preserving (`float -> double`, `i32 -> i64`, `u32 -> i64`), explicit cast for the rest, same N required |
| Generator ctor | `vec<float,4>([](auto i) { return i * 2.f; })` | None | gap | p4 candidate, low priority; `lanes()` + arithmetic covers iota-shaped cases |
| Per-lane literal init | No braced-lane ctor either (use generator or load from array) | `simd<float,4> v = {1,2,3,4};` refused (probed); load from a fixed array instead | agree (neither has it) | keep; the global-init message already points at `{...}`, which does not work - fix the message |
| Load ctor | `vec<T,N>(it, flags)` from a contiguous iterator | `simd<T,N>.load(arr, i)` static | equivalent | keep |

## 3. Operators

| Topic | C++26 | CFlat | Gap / divergence | Direction |
|---|---|---|---|---|
| Unary `+ -` | both, lane-wise | `-v` on float and int, `+v` on float (probed) | agree | keep |
| Unary `~` | integral only (ill-formed on float) | `~v` on `simd<i32,4>` = -7 (probed); `~v` on `simd<float,4>` CRASHES the verifier like `!v` (probed) | BUG on float: verifier failure, no diagnostic | refuse `~` on float lanes with a message |
| Unary `!` | returns `mask_type` (`!v` = `v == 0`) | `!m` on a mask works; `!v` on `simd<float,4>` CRASHES the verifier ("Logical operators only work with integral types", probed) | BUG: an LLVM verifier failure instead of a diagnostic (CLAUDE.md: assert -> add an error) | fix: either lower `!v` as `v == 0` (C++26) or refuse with a message |
| `++` / `--` | Member prefix and postfix, every element +-1, any value_type with `++` (float qualifies) | Ruled 2026-10-31 lane-wise. Today: `simd<i32,4> v; v++; v--;` already work (probed, 6 and 4); float/double `v++`/`v--` still fail with the lane-count message (probed). Prefix `++v` is a parse error (CFlat is postfix-only by design) | Half-built: the ruling is the spec, the float path is the gap | implement per the ruling (issue file already holds it) |
| Binary `+ - * /` | yes, vec op vec, vec op scalar via broadcast | yes, both directions (probed) | agree | keep |
| `%` | integral only | refused "other operators are not yet supported" (probed) | GAP | p4: `%` on integer lanes (`srem`/`urem` by signedness) |
| `<< >>` | integral; shift by vec or by a scalar count | refused (probed) | GAP | p4: shifts, scalar or vector count, `shl`/`ashr`/`lshr` by signedness |
| `& \| ^` | integral and masks | refused on vec and on masks (probed) | GAP, and the mask one blocks compound predicates | p4: bitwise on integer lanes AND on masks (`(a < b) & (c < d)`) |
| Compound `+= -= *= /=` | all arithmetic compounds | `v += 2.0`, `v *= w` work (probed); `%= <<= >>= &= \|= ^=` follow the missing binary ops | agree where the binary op exists | lands with the p4 above |
| Comparison | `== != < <= > >=` -> `mask_type`; float uses ordered compares | same six, `simd<bool,N>` result; float ordered except `!=` unordered (so a NaN lane is `!=`); integer signedness honoured | agree (C++ `!=` on NaN is also true) | keep |
| Comparison with a scalar | via broadcast | `v == 1.0`, `v < 2.0` work (probed) | agree | keep |
| Vector in a condition | ill-formed (mask has no bool conversion) | refused "the condition must be a single value ... compare it explicitly" for `if (a == b)` and `?:` (probed) | agree | keep |

## 4. Mask type

| Topic | C++26 | CFlat | Gap / divergence | Direction |
|---|---|---|---|---|
| Type | `mask<T, N>` (`basic_mask<Bytes, Abi>`), distinct from `vec` | `simd<bool,N>` - the same spelling family, lowers to `<N x i1>` | Spelling only | keep |
| Construct | broadcast from `bool`, generator, load from `bool*` | `simd<bool,4> m = true;` splats (probed); comparisons; no load | minor gap (mask load) | keep; `load` on `simd<bool,N>` could come with bool arrays later |
| Element read | `m[i]` -> bool | `m[i]` -> bool (probed) | agree | keep |
| Logical | `! && \|\| & \| ^`, compounds | only `!` and `== !=` (probed); `&`/`\|` refused | GAP (see bitwise row) | p4 above |
| Arithmetic on a mask | `+m`, `-m`, `~m` return an INTEGER vec (`-m` = 0/-1 per lane); `m + m` ill-formed | `m + m` = 0 (i1 add wraps, probed), `-m` = 1 (probed), both silently typed as masks | Silent divergence: a C++ programmer expects `-m` to be the 0/-1 integer blend idiom | refuse `+ - * /` on `simd<bool,N>` (doc already says you "cannot"); optionally give `-m` the C++ meaning later |
| Reductions | `all_of any_of none_of reduce_count reduce_min_index reduce_max_index` | None; `reduce_*` refuse bool lanes; idiom today is `m[0] \|\| m[1] ...` or `select` to 0/1 and `reduce_add` | GAP, the most visible one for a kernel epilogue | p4: `simd<bool,N>.any(m)`, `.all(m)`, `.none(m)`, `.count(m)` (C++26 names: `any_of/all_of/none_of/reduce_count`); lower to `vector.reduce.or/and/add` |

## 5. Conditional assignment, reductions, algorithms, math

| Topic | C++26 | CFlat | Gap / divergence | Direction |
|---|---|---|---|---|
| Select | `select(m, a, b)` free function (older drafts `simd_select`; the TS `where(m, v) = x` is gone) | `simd<T,N>.select(mask, a, b)`, any T, mask from any comparison (an `i32` select on a float-derived mask works, probed) | equivalent | keep |
| Masked assignment | `select(m, x, v)` idiom; masked `unchecked_store` | same idiom via `select` + `store_masked` | agree | keep |
| Reductions | `reduce(v, op)` with `plus<>` default, `reduce_min`, `reduce_max`, each with a masked overload + identity | `reduce_add` (float carries `reassoc`), `reduce_min`, `reduce_max`; no masked form, no `mul/and/or/xor` op | Partial; a masked reduce is one `select(m, v, identity)` away | p4 (low): `reduce_mul`, `reduce_and/or/xor` on integer lanes if a kernel asks; name stays `reduce_add` |
| Algorithms | `min(a,b)`, `max`, `minmax`, `clamp` for every vectorizable T | `.min/.max/.clamp` FLOAT ONLY (`simd<i32,4>.min` refused, probed); `.abs` float only | GAP on integer lanes | p4: integer `min/max/clamp/abs` (`smin/umin/smax/umax`, `llvm.abs`) |
| Math (`<cmath>` set) | Full set: sqrt, fma, abs, floor/ceil/trunc/round/rint/nearbyint, copysign, hypot, lerp, exp/log/sin/cos/pow/... | `sqrt abs fabs floor ceil trunc round rint min max clamp copysign sign fma`; transcendentals deliberately excluded (would scalarize) | Divergence by design; `sign` is a CFlat extra | keep; document that `exp/log/sin/...` are absent on purpose (already in HPC.md) |
| Bit ops ([simd.bit]) | `popcount`, `bit_ceil`, `countl_zero`, `byteswap`, ... on integer lanes | none | gap, low value until bitwise ops exist | after the bitwise p4 |

## 6. Element access, loads/stores, interop

| Topic | C++26 | CFlat | Gap / divergence | Direction |
|---|---|---|---|---|
| Subscript | `v[i]` const -> `value_type` by value; no lane write (the TS reference proxy was dropped) | `v[i]` read, runtime index OK (probed); lane write refused with a specific message; `v[0]++` refused (probed) | agree | keep |
| Loads | ctor from iterator with `flag_default / flag_aligned / flag_overaligned<N> / flag_convert`; `unchecked_load(range)` (UB if short), `partial_load(range)` (zero-fills the tail); masked overloads | `.load(arr, i)` element-aligned; `.load_masked(arr, i, mask, passthru)`; gather | No aligned flag, no `flag_convert` (load `float[]` into `simd<double,N>`), no partial load by count | p4 (small): `partial_load` is a mask + `lanes()` today - fine; `flag_convert` follows the element-conversion p4 |
| Stores | `unchecked_store`, `partial_store`, masked; same flags | `.store(v, arr, i)`, `.store_masked`, scatter | same shape | keep |
| Gather / scatter | Not in C++26 (permute via index vec only, P2664) | `.load_gather(arr, idxvec)`, `.store_scatter(v, arr, idxvec)` unmasked | CFlat extra | keep |
| Iota | `iota<V>` constant | `.lanes()` | equivalent | keep |
| Width change | `chunk<N>(v)` -> tuple of smaller vecs, `cat(a, b, ...)`, `resize<N>` | none ("converts only to the same lane count") | GAP | p4 (low): `simd<T,N>.cat(a, b)` / `.lo(v)` `.hi(v)`; wait for a kernel that needs it |
| Permute | `permute(v, index_map)` (P2664) | none ("shuffles are not yet supported") | gap | later; `shufflevector` is one intrinsic away |
| Type aliases | `rebind<U, V>`, `resize<N, V>` | none; the type spelling is also not accepted in a cast target, lambda parameter, or `function<>` signature (HPC.md) | gap, blocked by the parameter bug above | fix the parameter bug first |

## 7. Lists for the ruling

### (1) Gaps worth filing as `internal/issue/p4/` surface items (proposed spelling; not created)

1. Integer-lane `%`, `<< >>`, `& | ^` (and their compounds) on `simd<iN|uN, N>` - plain operators,
   `srem/urem`, `shl/ashr/lshr` by signedness; shift count scalar or vector.
2. Mask logic `& | ^` on `simd<bool,N>` (compound predicates); refuse `+ - * /` on masks.
3. Mask reductions: `simd<bool,N>.any(m)`, `.all(m)`, `.none(m)`, `.count(m)` (C++26
   `any_of/all_of/none_of/reduce_count`); lower to `llvm.vector.reduce.or/and/add`.
4. Element-type conversion at the same N: implicit when value-preserving (`float -> double`,
   `i32 -> i64`), explicit cast `(simd<U,N>)v` otherwise; plus `load` with conversion.
5. Integer `simd<T,N>.min/.max/.clamp/.abs` (today float only).
6. Lane-wise `v++`/`v--` on float/double lanes - already RULED 2026-10-31, listed for completeness.
7. Low priority: `reduce_mul/and/or/xor`, `cat`/`lo`/`hi` width change, `permute`, generator
   construction, `simd<T>` native width + a size query.

### (2) Silent divergences that would surprise a C++ programmer

- `vec<int8_t> + 300` / `vec<uint32_t> + -1` are ill-formed in C++26; CFlat truncates/wraps
  silently. CFlat scalars do the same (`i8 x = 300;` -> 44), so this is the scalar narrowing
  ruling's call, not a simd one.
- `-m` on a mask returns 1 (i1 negate), not the C++26 integer 0/-1 blend; `m + m` wraps to 0.
  Both compile today although HPC.md says a mask cannot be used arithmetically.
- `vec<float> + 1` is ill-formed in C++26 (int -> float not value-preserving); CFlat promotes.
- `!v` and `~v` on a float vector are LLVM verifier failures, not diagnostics (bug; C++26 `!v` is
  `v == 0`, `~v` is ill-formed).
- `simd<float|double|i32,N>` by-value function parameters silently lose the vector type at
  registration, so the call fails with "no overload matches" (bug; `u32` lanes work).
- `simd<T,N>*` is refused while `doc/HPC.md` says it is allowed (doc/behaviour mismatch).
- Non-power-of-two N and N=1 are errors (C++26 allows any N) - documented, deliberate.
- No lane-type conversion at all, even explicit - a C++ programmer expects at least `vec<int>(f)`.
