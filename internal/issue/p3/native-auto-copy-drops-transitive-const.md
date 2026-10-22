# Native transitive const lost when a pointer field is copied into an `auto` local

Severity p3 (false accept of a write native const semantics forbid). Found by the C05B r3 review (2026-10-20); pre-existing on master.
Repros: scratch/repro_keep/box1020/c05b_r3_preexisting/native_holder_local.cb, native_trailing.cb (explicitly deep-qualified clang twins
reject).

```cflat
struct N { int* p = nullptr; };
extern int main() { int n = 3; N x = { p = &n }; (const N)* c = &x; auto q = c->p; *q = 8; return 0; }   // accepted, should refuse
struct M { int* p = nullptr; int get() const { auto q = p; *q = 8; return *q; } };                       // accepted, should refuse
```
`*c->p = 8` directly is refused (transitive holder rule); the `auto` copy infers plain `int*` and drops the pointee const reached through
the const holder.

## Fix direction
`auto` inference from a field read through const storage (const pointer, trailing-const this) must carry the transitive levels into the
inferred slot's ConstMask (native const only - C++ const is shallow, see C05B). Explicitly typed `int* q = c->p;` should be refused as a
const-discarding conversion (check it does). Legs in Test/errors/ (an existing err_const_* file).

## PARKED 2026-10-20 (T348, 3 rounds; branch fix/t348-autoconst kept at r2 7fc77c557)
r2 (kept on the branch, Mac-green): at the auto-inference site (MainListener_Declarations.cpp ~9050) merge
`ValueConstLevels(rightNV.Primary)` when `FindConstWriteCause(rightNV.Storage)` reports a native (non-CxxShallow) cause. It fixes plain
field loads (`c->p`, trailing-const `p`, `(c->p)`, const param / generic holder, `int**` level 1). The Opus r1 review found it INCOMPLETE:
- the array-element field (`auto q = a.ps[0]`, `c->ps[0]`), `?:` (`f ? c->p : &m`) and arithmetic (`c->p + 1`) still drop the const;
  the direct write and the typed `int* q = ...` refuse all of them;
- `const N h = { p = new int(4) }; auto q = h.p; delete q;` becomes refused: delete of a `(const T)*` is refused on master too (clang
  accepts), so file/fix that with the item.
r3 (not kept): dropping the Storage/cause gate and always merging ValueConstLevels REGRESSES C++ shallow const (an auto copy through a
`R* const*` / const C++ holder becomes read-only, clang accepts) and still misses the array element. Do not retry that shape.
Next direction: compute the auto slot's mask from the same predicate the typed path uses (ConversionDiscardsConst,
LLVMBackend_MoveDataflow.cpp ~2293), so auto == typed by construction, with C++ shallowness decided there. Review matrix + repros:
scratch/repro_keep/box1020/t348_review/ (rev_t348_opus_probe.sh, cases, t348_review1.md).
