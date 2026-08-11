# `T**` still binds a `T*` parameter at the INTERFACE-DISPATCH door

## STATUS 2026-08-10: only the interface door is left, and it is WINDOWS-GATED.

Sections 1, 5 and 6 closed earlier. Sections 2, 3 (two of three doors), 4's value-side residue
and 7 closed in this change; the evidence is recorded below. The file stays open for ONE cell:
interface dispatch, which cannot be verified on the macOS host.

## STILL OPEN - interface dispatch (`ResolveInterfaceMethodSlot` / `CallInterfaceMethod`)

```cflat
class Circle { int r = 0; };
interface IUse { int use(Circle* c); };
class Impl : IUse { int use(Circle* c) { return 100 + c->r; } };
extern int main() { Circle* a = new Circle(); a->r = 7; Circle** pp = &a;
                    IUse i = new Impl(); return i.use(pp); }   // compiles; garbage (107 correct)
```

Measured on this branch's Release binary (`scratch/pdr/pdr_s3_iface_arg_pp.cb`): still compiles,
still runs, still reads the low bytes of the heap address (68 here, 100 on the pre-fix binary -
both garbage; exit codes track the process image, never pin one).

The pattern to copy is in place and used by the two doors that DID close:
`TypeAndValue::PointerDepthRefuses` plus the `PointerArgIntoByValueParam` /
`DiagnoseProvableInterfaceArgMismatch` shape in `LLVMBackend_WinRT.cpp`. Adding a
`PointerDepthRefuses` call inside `DiagnoseProvableInterfaceArgMismatch` is a two-line change.

**Precondition, unchanged: verify on WINDOWS first.** WinRT synthesizes interface parameters
without ever setting `ElemPointer`, so a parameter that is truly `T**` but recorded
`ElemPointer=false` would be refused - a Windows-only false rejection that a macOS host cannot
see. `x64/Release/cflat` on macOS never walks that synthesis path.

The accept side is already frozen: `pdr_s3_iface_arg_p.cb` (correct depth through the same
dispatch) reads 107 on both binaries, and `Test/test_basic.cb` carries the `pab_*` interface
argument legs.

**Second precondition, found in review and NOT live today:** the `--init` DESERIALIZER at
`LLVMBackend.cpp:4380` normalizes `FuncPtrParam.PointerDepth` 0 -> 1 whenever `Pointer` is set.
So a parameter whose depth is genuinely UNRECORDED is judged as a proven depth-1 on a warm cache
and skipped on a cold one - the two runs would disagree. Nothing depends on it now: only the
C-interop population leaves `PointerDepth` unset, and that never reaches the `--init` serializer,
which is why `FuncPtrArgDepthMismatch` (the `function<>` door) is safe as written. It becomes
load-bearing the moment the interface door is gated the same way, because the WinRT-synthesized
parameters this section is about are exactly the ones that leave the depth unrecorded. Decide it
together with the gate: either stop normalizing, or serialize an explicit "unrecorded" marker.

## ALSO OPEN - the PRIMITIVE twin of section 7 (`int*[N]` into an `int*` parameter)

Measured this round, not previously filed. `IsProvenDecayedDoublePointer` requires a non-empty
`TypeName`, and a primitive pointer array carries none (the argument renders as `ptr*`), so the
primitive twin of the closed section-7 cell is untouched:

```cflat
int f(int* p) { return 90 + p[0]; }
extern int main() { int*[2] arr = default; int x = 4; arr[0] = &x; return f(arr); }
```

Compiles and runs garbage on BOTH binaries (`scratch/pdr/sel_k.cb`; the two values differ only
because the warm-cache module layout moves the stack address it reads - the emitted call is
`@_f_i32_i32Ptr_` in both, and the lone-`int**` twin `sel_j.cb` reads the correct 194 on both).
With an overload set of `{int*, int**}` the selection is likewise unchanged pre and post
(`sel_i.cb`, `@_f_i32_i32Ptr_` both). Fix direction is one token - drop the `!TypeName.empty()`
requirement - but it must be measured first: `void*[N]` also renders with an empty `TypeName`, so
the `TypeName != "void"` exclusion that keeps the sink legal would stop protecting it.

## CLOSED 2026-08-10 - sections 2, 3 (operator + `function<>` doors), 4 (value side), 7

Every row measured pre (merge-base Release binary, kept at `scratch/pdr/cflat_pre`) and post
(this branch). Probe corpus: `scratch/pdr/pdr_*.cb`, driven by `scratch/pdr/run.sh`.

### 2. A PRIMITIVE pointer argument never reached the depth gate

Root cause was NOT that the scorer skipped `IsTypeMatch`. `IsTypeMatch` ran and refused the pair;
the scorer's **numeric fallback** then re-granted it, because `int**` and `int*` both spell the
32-bit `"int"` and that fallback consults only integer width and signedness. Fix: the depth half
of `IsTypeMatch` is split out as `TypeAndValue::PointerDepthRefuses` and applied as an OVERRIDE
after both scoring branches in `ComputeOverloadFunction`, so neither the numeric fallback nor the
empty-TypeName (`CompareUpconvert`) branch can resurrect a refused pair.

| cell | pre | post |
|------|-----|------|
| `deref(pp)`, `int deref(int*)`, `int** pp` | compiled, ran, garbage | refused, depth note |
| `take(pp)`, `char take(char*)`, `char** pp` | compiled, garbage | refused |
| `derefPP(p)`, `int derefPP(int**)`, `int* p` (mirror) | compiled, SIGSEGV | refused |
| `GB<int*>.put(pp)` (primitive through substitution) | compiled | refused |
| `deref(p)` / `deref(&x)` / `derefPP(pp)` / `derefPP(&p)` | 103 / 103 / 203 / 203 | identical |
| `GB<int**>.put(pp)` | 11 | 11 |
| `int**` into `void*`, `void**` into `void*`, `int*` into `void*` | 7 / 7 / 7 | identical |
| `printf("%p", pp)` (varargs declare no depth) | 5 | 5 |
| `deref(nullptr)` | 42 | 42 |

A primitive pointer argument carries an EMPTY CFlat `TypeName`, so the note cannot name a type;
it says "the argument has one more/fewer level of indirection" instead of printing `'**'`.

### 3. Two of the three argument-position doors

**Operator OPERAND** (`v + pp`): only the receiver reached the scorer - the operand was reduced to
a raw `llvm::Value` before `TryBinaryOperatorOverload` saw it, and under opaque pointers it then
matched any pointer parameter. `TypedValue` now carries `pointerDepth`/`elemPointer` (stamped in
`TypedValueOfNamedOperand`, only when `DepthIsAboutThisValue()`), threaded through
`TryBinaryOperatorOverload` and stamped onto the reconstructed right `NamedVariable` ONLY when
positive - so an unproven pointer still claims nothing. `TryPointerLhsOperatorOverload` is
structurally exempt: it returns early on a pointer-typed RHS, so no depth question reaches it.

**Indirect `function<>` call** (`f(pp)`): the `[PFX-5]` site lowers its own argument list and
enters neither the scorer nor `ResolveInterfaceMethodSlot`. New `LLVMBackend::FuncPtrArgDepthMismatch`
judges each argument against `FuncPtrParams[i]`; it requires a POSITIVE `PointerDepth` on the
parameter, because 0 means "not recorded" for every synthesized signature (C interop, WinRT) and
for the substituted-component path section 5 left collapsed.

| cell | pre | post |
|------|-----|------|
| `v + pp` into `operator+(Circle*)` | compiled, garbage 228 | refused |
| `v + a` into `operator+(Circle**)` (mirror) | compiled, SIGSEGV | refused |
| both overloads declared, `(v+a)+(v+pp)` | SIGSEGV | **58** (= 107+207, correct) |
| `f(pp)`, `function<int(Circle*)> f = byPtr` | compiled, garbage 180 | refused |
| `f(pp)` where `f` is a LAMBDA value | compiled, garbage 36 | refused |
| `f(a)`, `function<int(Circle**)>` (mirror) | SIGSEGV | refused |
| `f(&a)` (inline `&`, depth 2 into depth 1) | compiled, garbage 116 | refused |
| `v + a` / `v + &c` / `v + nullptr` | 107 / 107 / 44 | identical |
| `v + pp` into `operator+(Circle**)` | 107 | 107 |
| `f(a)` / `f(nullptr)` / `f(41)` / lambda `f(a)` | 107 / 44 / 42 / 107 | identical |
| `f(pp)`, `function<int(Circle**)>` | 207 | 207 |

### 4. A depth-3 VALUE needs no declarator

`&` over an operand already PROVEN at the cap produces a `T***` value the two-level model cannot
represent. Following section 6's ruling for written stars, it is refused **where it is produced**
- at the `&` - rather than clamped back to `T**` and judged as a lie at every use site. An operand
whose depth was never recorded still records nothing, so `&` invents no claim.

| cell | pre | post |
|------|-----|------|
| `byPP(&pp)`, `Circle** pp` | compiled, garbage | refused at `&` |
| `Circle** q = &pp;` (no call at all) | compiled, garbage | refused at `&` |
| `&h.pp` over a `Circle**` FIELD | compiled, garbage | refused, names `'h.pp'` |
| `derefPP(&pp)`, `int** pp` (primitive) | compiled, garbage | refused at `&` |
| `&pp` into `void*` / into varargs | 7 / 5 | refused (the cap is about the VALUE, not its use) |
| `byPtr(&pp)` (depth 3 into depth 1) | already refused by the scorer | refused at `&`, one stage earlier |
| `&c` (depth 1) / `&a` (depth 2) / `&arr[0]` / `&h.p` / `&intArr[0]` | 107 / 207 / 207 / 207 / 107 | identical |

### 7. `T*[N]` decays to `T**`, so binding a `T*` is the depth mistake

Measured FIRST, before any guard: plain array decay is a sanctioned spelling. `int[2]` into
`int*` reads 97, `Circle[2]` into `Circle*` reads 97, `int[]` into `int*` reads 97 - all correct.
So `Circle*[2]` decaying to `Circle**` is the CORRECT decay (it already read 97), and binding a
`Circle*` is the mistake. New `TypeAndValue::IsProvenDecayedDoublePointer` claims depth 2 for a
fixed array whose ELEMENT is a plain pointer, and nothing else: a view claims nothing, a
non-pointer element claims nothing, `ElemPointer` on the element would be depth 3.

| cell | pre | post |
|------|-----|------|
| `firstR(arr)`, `Circle*[2] arr`, `int firstR(Circle*)` | compiled, garbage 186 | refused, note names the decay |
| `firstRR(arr)`, `int firstRR(Circle**)` | 97 (correct) | 97 |
| `pick(arr)` with BOTH `pick(Circle*)` and `pick(Circle**)` declared | took the `T*` body (garbage) | takes the `T**` body (194) |
| same, with the `Circle**` overload declared AFTER the call site | took the `T*` body (42) | takes the `T**` body (194) |
| `pick(arr)` with `{Circle*, void*}` | `@_f_i32_CPtr_` (garbage 26) | `@_f_i32_U8Ptr_` (55, the sink) |
| `pick(arr)` with `{Circle**, void*}` | 194 | 194 |
| `pick(&arr[0])` with `{Circle*, Circle**}` | 194 | 194 |
| generic `f<T>(T*)` / `f<T>(T**)`, `f<Circle>(arr)` | 194 | 194 |
| `Circle*[2]` as a PARAMETER | refused at the declaration ("pass 'Circle**'") | unchanged |
| `sum(intArr)` / `firstR(structArr)` / `sum(view)` | 97 / 97 / 97 | identical |
| `firstR(arr[0])` (element, not array) | 97 | 97 |
| `printf("%p", arr)` | 5 | 5 |
| `firstR(b.get())`, `GB<Circle*>` | 97 | 97 |
| `firstV(arr)` into a `Circle*[]` VIEW param | already refused (view must span an allocation) | unchanged |

### Verification

- `./test.sh Release`: 646 passed, 0 failed, 8 skipped. `bash example_mac.sh Release`: 35/0.
- Differential `--check` sweep of all 453 files in `Test/`, `Test/errors/` and `example/`, pre-fix
  binary vs this one: ZERO divergence outside the two edited test files.
- Legs: `Test/test_basic.cb` (`pd_prim_*`, `pd_*_decays_*`, `pd_ptr_array_picks_ptrptr_overload`, `pd_fnptr_*`, `pd_op_operand_*`,
  `pd_addrof_ptr_field_*`) for the accept sets; `Test/errors/err_double_pointer_arg_single_pointer_param.cb`
  and its mirror for the ten new refusals. The whole of `test_basic.cb` SIGSEGVs on the pre-fix
  binary at the operator-operand leg, and every new `expect_error` leg has a measured pre-fix
  ACCEPT twin in `scratch/pdr/`.
