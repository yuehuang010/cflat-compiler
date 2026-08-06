# A data pointer ASSIGNED to a thin `function<>` local or field compiles clean and SIGBUSes

Filed 2026-08-05 by `fix/genfn-lowering` while freezing the accept set for generic instantiations
over closure types. **Pre-existing and not generic-specific** - the plainest spelling, a bare
local, has it. Measured IDENTICAL on the pre-fix binary (master `8c5a860`) and on the post-fix
binary, so `fix/genfn-lowering` neither caused nor widened it.

Severity: memory-unsafe accept. Compiles with no diagnostic, then calls a DATA address as code.

## Repro - both spellings, both binaries

```cflat
import "function.cb";
int q = 3;
extern int main() { function<int(int)> f = default; void* vp = &q; f = vp; printf("call=%d\n", f(1)); return 0; }
```

```cflat
import "function.cb";
struct S { function<int(int)> f = default; };
int q = 3;
extern int main() { S s = default; void* vp = &q; s.f = vp; printf("call=%d\n", s.f(1)); return 0; }
```

Both: compile exit 0, no diagnostic; the program exits **138** (SIGBUS) with no output on
`8c5a860` and on `fix/genfn-lowering`.

## Why the existing gates miss it

The provenance gate (`WidenToClosureFatChecked` / `CheckThinFnPtrArgProvenance`, `ce9858e`) runs
on ARGUMENT positions only. The store-side gate that landed as `fix/codeval-store`
(`CodeValueIntoDataDestination`) answers the OPPOSITE question - a code value flowing into a data
destination - so neither one looks at a data value flowing into a thin CODE destination at an
assignment. The FAT twin is rejected by accident rather than by a gate: `Lambda<>` storage is a
struct, so `s.f = vp` trips the generic "cannot store a pointer value into struct storage"
message. A thin `function<>` slot is a bare pointer, so nothing objects.

The generic spelling `Box<function<int(int)>>.item = vp` behaves the same, by design: after
`fix/genfn-lowering` a thin encoded element IS the same bare code pointer as the spelled type, so
it inherits this hole exactly and no more. Its FAT counterpart `Box<Lambda<>>.item = vp` IS now
gated (a located provenance error), because that path was rewritten to go through
`WidenToClosureFatChecked`.

## Fix direction

Run `ArgumentIsProvablyDataPointer` on the assignment RHS when the destination is a thin
`function<>` (spelled or encoded) - the same predicate, the same wording as the thin parameter
message. Build the accept set FIRST: a named function, a `function<>` value, a `?:` join of
those, `nullptr`, and an explicit `(function<...>)value` cast must all keep compiling.

## Test coverage

None for this direction. The argument direction is covered by
`Test/errors/err_data_pointer_to_closure_param.cb`, which is where an assignment leg belongs.

Related: [[interface-issue-queue]]. The RETURN-path sibling
(`data-pointer-returned-as-closure-not-gated`) was fixed and deleted 2026-08-06 by
`fix/return-gate` - see its landed record in the queue file.
