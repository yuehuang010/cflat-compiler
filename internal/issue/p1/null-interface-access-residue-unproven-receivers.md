# Residue of the null-interface-access fix: receivers the straight-line proof cannot reach

Residue of `interface-method-call-on-null-value-segfaults`, closed by the commit that added
the deferred definitely-null proof (`RunNullIfaceDispatchCheck` in `cflat/LLVMBackend.h`, with
record sites for the method dispatch and the interface-field lvalue in `cflat/MainListener.h`).

That commit rejects a plain `.` method call OR field access whose receiver is a named local's
own frame slot, when the last write to that slot before the access - in the access's own basic
block - is a whole-slot store of a null constant. The shapes below still compile clean and
SIGSEGV with no diagnostic, exactly as they did before the commit. **Not a regression:** every
one of them behaves identically on the pre-fix binary.

Severity: SIGSEGV at run time (exit 139) with a clean compile and no diagnostic - same class as
the closed issue, and the reason a tracked record has to survive it.

Measured on `/Users/felixhuang/source/cflat-compiler/x64/Release/cflat` (master, no fix) and on
the fix branch. Shared preamble for every repro:

```cflat
interface PLive { int tag; int Get(); };
class PImpl : PLive { int tag = default; int d = default; int Get() { return d; } };
struct PHolder { PLive c = default; };
PLive gLv = default;
```

## Headline - one pair of parentheses defeats the FIELD diagnostic, but not the METHOD one

```cflat
extern int main() { PLive lv = default; printf("%d\n", (int)(lv).tag); return 0; }
```

| spelling | master | fix branch |
|---|---|---|
| `(lv).tag` | compiles, exit 139 | compiles, exit 139 - **undiagnosed** |
| `lv.tag`   | compiles, exit 139 | rejected: `member access on null interface value 'lv'` |
| `(lv).Get()` | compiles, exit 139 | rejected: `method call on null interface value 'lv'` |

The asymmetry is structural, not accidental. On the METHOD path the receiver's slot pointer is
handed to `CallInterfaceMethod`, which always emits its own vtable load at the dispatch point,
so the anchor exists whatever the spelling. On the FIELD path the anchor IS the read - the
single `LoadInst` of the fat value - and the record is gated on `interfaceVar.Primary ==
nullptr`, i.e. on that load being emitted here and now. Parenthesising the receiver leaves the
fat value already in `Primary` (loaded in an earlier walk step, possibly in an earlier block),
so nothing is recorded and the access compiles.

## Other unproven receivers - all 139 on BOTH binaries, method and field alike

```cflat
extern int main() { PHolder h = default; printf("%d\n", (int)h.c.Get()); return 0; }   // struct field
extern int main() { PHolder h = default; printf("%d\n", (int)h.c.tag);   return 0; }
extern int main() { PLive[2] a = default; printf("%d\n", (int)a[0].Get()); return 0; } // array element
extern int main() { PLive[2] a = default; printf("%d\n", (int)a[0].tag);   return 0; }
extern int main() { printf("%d\n", (int)gLv.Get()); return 0; }                        // global
extern int main() { printf("%d\n", (int)gLv.tag);   return 0; }
```

All six: compile rc 0, run rc 139, on master and on the fix branch. A struct field and an array
element resolve through a GEP and a global through a `GlobalVariable`, so none of them is the
`AllocaInst` the record requires.

## NOT residue - deliberately accepted, do not "close" these

Per the maintainer's ratified design (no per-dispatch runtime guard; `?.` is the language's
answer wherever liveness is not statically known), the following keep compiling ON PURPOSE and
must not be turned into rejections:

- `?.` in either spelling (`lv?.Get()`, `lv?.tag`) - the sanctioned remedy, short-circuits.
- a receiver assigned in a branch, in a loop body, or anywhere other than the access's own
  straight-line block.
- an access sitting under a branch that is false at run time (`if (pick(0) == 1) { lv.Get(); }`).
- a parameter receiver - its slot's only write is the incoming argument, never a null constant.
- an access inside a folded-away `if const` arm (`if const (sizeof(int) == 99) { lv.Get(); }`)
  - verified compiling on the fix branch.

The accept set for all of these is `testNullIfaceDispatchAcceptSet` in `Test/test_interface.cb`.

## Fix direction - the parens case is the RISKY one; the gate exists on purpose

Closing `(lv).tag` means recording when `interfaceVar.Primary != nullptr`, which today is
declined outright. Doing it safely needs a proof the current code gets for free and would then
have to earn: that the load backing `Primary` lives in the SAME basic block as the access, and
that no write to the slot sits between them. Anchoring on any instruction emitted in the
current block instead would make the walk inspect stores that happened AFTER the fat value was
read - a null store in the current block following an earlier non-null load would become a
FALSE REJECTION of a working program.

Guard polarity is the load-bearing constraint here (`internal/fix-issue-lessons.md`): the
`Primary == nullptr` gate degrades to "no diagnostic", which is the correct direction. A false
rejection would be strictly worse than the SIGSEGV this residue describes. Anyone picking this
up should carry `Primary`'s defining `LoadInst` through and reject only when
`load->getParent() == access->getParent()`, rather than widening the anchor.

The struct-field / array-element / global receivers are a larger job: each needs its own
"definitely null at this point" proof over storage that is not a frame slot, and for a global
that proof spans translation units. There is no cheap version of it, and no evidence anyone has
hit it in real code - `core/` and `example/` contain no such shape.
