# One intermediate local defeats the interface return-dangle guard

Filed 2026-07-28 by an adversarial review of the stack-value `as` fix.
PRE-EXISTING: both spellings behave identically, so this is not something `as` introduced.

Severity: accepted source, no diagnostic, returns a pointer into the dead frame.

## Repro

```cflat
interface IShape { int area(); };
class Square : IShape { int s = 0; int area() { return s * s; } };

// Both of these compile clean and both dangle.
IShape viaCast(int n)  { Square loc; loc.s = n; IShape r = loc as IShape; return r; }
IShape viaPlain(int n) { Square loc; loc.s = n; IShape r = loc;           return r; }

int clobber() { int[64] a; int i = 0; while (i < 64) { a[i] = 0x5A5A5A; i = i + 1; } return a[7]; }

extern int main()
{
    IShape v = viaCast(7);
    int j = clobber();
    printf("area=%d (expect 49) j=%d\n", v.area(), j);   // prints garbage
    return 0;
}
```

Prints `area=-1490327644`. Returning the same expression directly - `return loc as IShape;`
or `return loc;` - is correctly rejected.

## Root cause

The return-path frame-lifetime check (`FrameLocalDataOfFatValue`, `MainListener.h:5066`)
inspects the returned VALUE. It follows `insertvalue` chains and `?:` joins back to the data
pointer that was boxed, and it deliberately stops at a load: tracing through loads is what
would make heap and by-reference shapes look frame-local and produce false rejections.

Binding the boxed value to a local first means the returned expression is a plain load of
that local's slot, so the walk bails on the first instruction and the guard never engages.
Closing this needs store-to-slot reasoning (which store reaches this load), not a deeper
value walk.

## Practical consequence

A user who hits the return-dangle error can make it disappear by inserting a local:

```cflat
IShape f() { Square s; return s as IShape; }              // rejected
IShape f() { Square s; IShape r = s as IShape; return r; } // accepted, still dangles
```

That is the worst shape for a diagnostic to have - it teaches the wrong workaround. Worth
weighting when prioritising this.

## ATTEMPTED 2026-07-29 AND ABANDONED - read this before trying again

Three analyses were written against the "Fix direction" below. All three passed the full
suite (512/0/8) and all three were caught by adversarial review REJECTING LEGAL PROGRAMS.
The attempt is preserved on branch `fix/return-dangle-provenance` (commit `446f028`,
based on `4b045a4`); it was never merged. Repros are in that worktree under `scratch/rev*/`.

| # | How the reaching store was chosen | Legal programs it rejected |
|---|-----------------------------------|----------------------------|
| 1 | Last store in LAYOUT order | `if`/`else` with the return in the `else`; early return out of a `then`; return inside a `while` body |
| 2 | Nearest store that DOMINATES the return | Both arms overwrite the slot; the `switch` form; nested `if`/`else` where every leaf overwrites; an `if`/`else if` chain |
| 3 | Backward walk stopping at the first store on each path (kill-aware may-reach), rejecting only when EVERY reaching value is a frame box | `return <interface local>` lexically inside any loop - `for`, `while`, `do`/`while`, nested, and independent of `move` |

Each analysis fixed the previous one's counterexamples and introduced its own. Attempt 3's
failure is the important one, because it is STRUCTURAL rather than a smarter-query problem:

**The guard is computed while the function is still being emitted.** At a `return` inside a
loop body, stores later in the body do not exist yet and the latch->header back-edge is not
wired. Attempt 3 assumed missing CFG information is monotone toward acceptance. Under the
all-frame rejection rule it is not - dropping the NON-frame store is exactly what makes the
predicate fire. Missing information causes a false rejection.

That rule is itself forced, not a free choice. Rejecting when ANY frame box may reach breaks
`if (c > 0) {...} else if (c <= 0) {...}`, which is exhaustive by arithmetic but not by CFG,
so the emitted graph carries an edge delivering the pre-`if` frame box to the return. That
shape is CFG-indistinguishable from a real conditional dangle; only path-sensitive reasoning
about the conditions separates them.

**Any future attempt must run over a COMPLETE CFG** - a post-emission pass over the finished
function - or decline to answer whenever the walk could still be missing an edge or a store.
A query that is precise at emission time cannot be sound in the rejecting direction.

Two further lessons, both cheap to re-learn the hard way:

- **A green suite proves nothing here.** All three attempts were 512/0/8. No in-repo `.cb`
  used the shapes that broke. Any retry must add POSITIVE tests - legal programs that must
  keep compiling - not just `expect_error` legs, and must verify them against the CURRENT
  compiler too, since they assert behaviour both binaries share.
- **The governing asymmetry**: a false rejection is a blocker, a missed dangle is merely
  today's behaviour. When the analysis is unsure, ACCEPT.

Two pieces of the attempt were salvaged and have ALREADY LANDED on master, so do not redo
them: the `ClassifyInterfaceBoxSource` ordering fix (storage shape is now tested before
ownership) and the provenance filter on `FindInterfaceBoxByDataPointer`. See
[[interface-boxing-sites-not-fully-consolidated]] for what they did and what they left open.

## Fix direction

> The claim below that this is "only a wiring one" is WRONG - see the abandoned-attempt
> section above. The ledger is necessary but not sufficient: it answers what a value IS,
> while the hard part is which store REACHES the return, and that cannot be answered
> soundly at emission time. Keep the ledger; discard the "just look it up" framing.

**The prerequisite now EXISTS - this is no longer a design problem, only a wiring one.**
The boxing consolidation added exactly the thing this issue asked for: a provenance ledger,
`LLVMBackend::interfaceBoxRecords_`, written by `BoxConcreteIntoInterface`
(`MainListener.h:9969`) and keyed on BOTH the produced fat value and its data half. Each
record carries `{FatValue, DataPointer, SourceClassName, InterfaceName, Source,
OwnershipTransferred}` where `Source` is one of Unknown / FrameStorage / Heap / Parameter /
Global.

It is deliberately NOT retired by `FlushOwnedTemps` - unlike its sibling ledgers - precisely
so a record written at the declaration of `r` is still there at the `return` in a later
statement, which is this issue's shape.

So the fix is: at the return path, look the returned value up in the ledger instead of
walking IR. A record with `Source == FrameStorage` is the dangle, whether the value came
straight from the boxing site or through an intermediate local. That answers
`IShape r = loc as IShape; return r;` by construction, because the fact was recorded where
the boxing happened rather than recovered from the shape of the returned expression.

`FrameLocalDataOfFatValue` (`MainListener.h:5129`) can then be reduced to a fallback for
values with no ledger record (a fat pointer that arrived from a call, say), or removed if
the ledger proves complete.

**Read [[interface-boxing-sites-not-fully-consolidated]] BEFORE starting.** This change adds
the ledger's SECOND consumer, and unlike the first it will not sit behind the
`FrameLocalDataOfFatValue(right) == nullptr` gate that made the ledger's sharp edges
unreachable. Both edges recorded there have since been addressed on master - the classifier
ordering is FIXED, and the data-pointer lookup is now provenance-filtered - so this work no
longer has to resolve them first. One residue remains and a second consumer must not assume
otherwise: `RegisterInterfaceBox` still dedupes on `FatValue` alone, so two records sharing
both a data pointer and a `Source` resolve first-registered-wins.

Note also that the ledger only has a record when the source had a NamedVariable - see
[[interface-boxing-guards-are-binding-dependent]] for the shapes that produce no binding.

## Related

- [[interface-boxing-sites-not-fully-consolidated]] - REQUIRED READING; the ledger sharp
  edges this change would expose.
- [[interface-boxing-guards-are-binding-dependent]] - the shapes the ledger cannot see.
- The two `as`-boxing issues this used to link (ownership transfer, pointer-shape rejection)
  are FIXED; see the closed section of [[interface-issue-queue]].
