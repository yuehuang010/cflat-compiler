# Residue of the null-interface-access fix: non-frame-slot receivers

Residue of `interface-method-call-on-null-value-segfaults`, closed by the commit that added
the deferred definitely-null proof (`RunNullIfaceDispatchCheck` in `cflat/LLVMBackend.h`, with
record sites for the method dispatch and the interface-field lvalue in `cflat/MainListener.h`).

That commit rejects a plain `.` method call OR field access whose receiver is a named local's
own frame slot, when the last write to that slot before the access - in the access's own basic
block - is a whole-slot store of a null constant. The shapes below still compile clean and
SIGSEGV with no diagnostic. **Not a regression:** every one of them behaves identically on the
pre-fix binary.

Severity: SIGSEGV at run time (exit 139) with a clean compile and no diagnostic - same class as
the closed issue, and the reason a tracked record has to survive it.

Shared preamble for every repro:

```cflat
interface PLive { int tag; int Get(); };
class PImpl : PLive { int tag = default; int d = default; int Get() { return d; } };
struct PHolder { PLive c = default; };
PLive gLv = default;
```

## CLOSED - the parenthesized named-local receiver

`(lv).tag` was the one remaining spelling on a NAMED LOCAL that escaped the proof: parenthesising
left the fat value already in `Primary`, and the field record was gated on emitting the load here
and now. Closed by anchoring on `Primary`'s defining `LoadInst` when that load reads the same slot
in the same basic block as the access - the access consumes the loaded value, so stores after the
load cannot change what faults, and a load from an earlier block is still declined. Measured
before/after on `(lv).tag`, `((lv)).tag`, `(((lv))).tag`, `(lv).tag = 5`, and the same shapes after
a mid-block `lv = nullptr;`: all rejected now, all `exit 139` before. The method path
(`(lv).Get()`) already rejected in every paren spelling and is unchanged. Regression legs live in
`Test/errors/err_iface_field_missing.cb`; value-asserting companion accepts are legs 13-20 of
`testNullIfaceDispatchAcceptSet` in `Test/test_interface.cb`. Those accepts are NOT tripwires for
a widened anchor: a build that drops all four anchor conditions and reloads at the access point
still passes the whole suite 554/0/8, so the guard's narrowness rests on the argument above, not
on test coverage. Closing that gap needs a witness where a null store sits between the anchor load
and the access, and no such shape is currently reachable.

## STILL OPEN - struct field, array element, and global receivers

```cflat
extern int main() { PHolder h = default; printf("%d\n", (int)h.c.Get()); return 0; }   // struct field
extern int main() { PHolder h = default; printf("%d\n", (int)h.c.tag);   return 0; }
extern int main() { PHolder h = default; printf("%d\n", (int)(h.c).tag); return 0; }
extern int main() { PLive[2] a = default; printf("%d\n", (int)a[0].Get()); return 0; } // array element
extern int main() { PLive[2] a = default; printf("%d\n", (int)a[0].tag);   return 0; }
extern int main() { printf("%d\n", (int)gLv.Get()); return 0; }                        // global
extern int main() { printf("%d\n", (int)gLv.tag);   return 0; }
extern int main() { printf("%d\n", (int)(gLv).tag); return 0; }
```

All eight: compile rc 0, run rc 139 - measured unchanged by the parens fix, and identical on
master. A struct field and an array element resolve through a GEP and a global through a
`GlobalVariable`, so none of them is the `AllocaInst` the record requires.

Fixing them is a larger job: each needs its own "definitely null at this point" proof over storage
that is not a frame slot, and for a global that proof spans translation units. There is no cheap
version of it, and no evidence anyone has hit it in real code - `core/` and `example/` contain no
such shape.

## NOT residue - deliberately accepted, do not "close" these

Per the maintainer's ratified design (no per-dispatch runtime guard; `?.` is the language's
answer wherever liveness is not statically known), the following keep compiling ON PURPOSE and
must not be turned into rejections - in the plain spelling AND the parenthesized one:

- `?.` (`lv?.Get()`, `lv?.tag`, `(lv)?.tag`) - the sanctioned remedy, short-circuits.
- a receiver assigned in a branch, in a loop body, or anywhere other than the access's own
  straight-line block.
- an access sitting under a branch that is false at run time (`if (pick(0) == 1) { lv.Get(); }`).
- a parameter receiver - its slot's only write is the incoming argument, never a null constant.
- an access inside a folded-away `if const` arm (`if const (sizeof(int) == 99) { lv.Get(); }`).
- a read whose live value was loaded BEFORE a null store later in the same block
  (`lv = s; int t = (lv).tag; lv = nullptr;`) - the anchor is the load, not the access.

The accept set for all of these is `testNullIfaceDispatchAcceptSet` in `Test/test_interface.cb`.

Guard polarity remains the load-bearing constraint (`internal/fix-issue-lessons.md`): every gate
here degrades to "no diagnostic", which is the correct direction. A false rejection would be
strictly worse than the SIGSEGV this residue describes.
