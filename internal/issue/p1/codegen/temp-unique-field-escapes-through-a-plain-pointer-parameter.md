# A temp's `unique` field still escapes through a PLAIN `T*` parameter

Filed 2026-08-05 by `fix/tempuniq`, which closed the cast, join, array-aggregate and
`unique`/`move`-parameter spellings of
[[temp-unique-field-escapes-through-unguarded-spellings]] and DELETES that file. This is the
remainder that issue already named as undecidable at the call site, split out so the P1 could
close without the remainder closing with it.

Severity: **silent use-after-free**. Compiles clean, runs, prints a plausible value, exits 0.
P2 rather than P1 under the residue-not-regression precedent
([[unique-field-to-field-interface-receiver-residues]],
[[return-dangle-missed-when-slot-has-extra-user]]): the spelling was accepted before the fix
too, so this is residue rather than regression. **Re-rank to P1 if the maintainer rules the
memory-unsafe-accept rubric wins** - it is the same rubric that made the parent issue a P1.

## The repro

`Node` has NO destructor throughout; `scratch/tu/cells/*__argplain.cb` in the fix worktree
holds every measured spelling.

```cflat
struct Node { int v = 0; };
struct Box<T> { T t = default; };
Box<unique Node*> makeBox() { Box<unique Node*> b = default; b.t = new Node(); b.t->v = 70; return b; }

Node* g = nullptr;
void keep(Node* n) { g = n; }        // a PLAIN `Node*` parameter that STORES

extern int main()
{
    keep(makeBox().t);
    printf("v=%d\n", g->v);          // freed-then-read: garbage or a reused block, not 70
    return 0;
}
```

Measured `-o` + run (never `--check`) on master `14097e1` AND on the merged `fix/tempuniq`:
**rc 0, freed-then-read on both** - `dtors=1` proves the temp's destructor freed the pointee,
and an allocator-reuse witness (`Node* fresh = new Node(); fresh->v = 99;` then `raw` reads 99,
`same=1`) proves the read aliases the reallocated block. The `MallocScribble=1` 0x55 fill
(`v=1431655765`) is visible only in an ld64.lld-linked build; a `Linking (mach-o)` build shows
the reuse value instead - do not use the fill as the discriminator across differently-linked
binaries. All ten source spellings behave identically - bare, parenthesized, same-type cast,
cast-of-paren, `??`, `?:`, `?:` with two temp arms, cast-of-join, join-of-cast, paren-of-cast.

Four call shapes reach it, and they are all one thing - a plain `T*` parameter:

| Spelling | Parameter that receives it |
|---|---|
| `keep(makeBox().t)` | `void keep(Node* n)` |
| `l.add(makeBox().t)` | `list<Node*>::add(Node*)` |
| `PlainSlot(makeBox().t)` | a constructor's `Node* q` |
| `keepg(makeBox().t)` where the body stores to a global | `void keepg(Node* n)` |

## Why it is not closed

The store happens in the CALLEE, so a call site cannot in general tell a storing argument from
a read-only one - and the read-only spelling MUST keep working:

```cflat
int rd(Node* n) { return n->v; }
rd(makeBox().t)                      // correct: consumed before end of statement
```

That accept is frozen as `temp_uniq_accept_plain_param_read` and
`temp_uniq_accept_plain_param_cast_read` in `Test/test_move.cb`, with destructor counts, so
any future guard here has its accept cell already written.

`fix/tempuniq` closed the two parameter spellings that STATE the claim at the call site
(`unique T*` and `move T*`, `RejectOwningTempUniqueFieldIntoSinkParam` in
`cflat/LLVMBackend.h`); the plain one states nothing.

## Fix direction

Not a call-site predicate. The decidable version needs a CALLEE-side fact - "this parameter is
stored into something that outlives the call" - computed once per function and read at every
call site, i.e. the record-then-resolve shape this repo already uses twice
(`codeValues_`, `owningTempUniqueFields_`). The escape analysis in `LLVMBackend.h`
(`RegisterNonEscapingOwningPtrArgs`, `FunctionBodyIsComplete`) already answers a very close
question for owning pointer arguments and is the obvious place to start - note it is
order-sensitive on the callee being complete, which is why the parent issue called this
"a real design step, not a call".

Related: [[interface-issue-queue]]
