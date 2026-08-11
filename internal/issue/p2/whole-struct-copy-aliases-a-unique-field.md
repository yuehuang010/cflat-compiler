# A whole-struct copy of a struct holding a `unique` field aliases the owner

Measured 2026-08-10 while verifying the uniform-implicit-move ruling on `fix/uniq-implicit-move`,
and measured IDENTICALLY on the merge base `c9405da` - residue, not regression. Probe:
`scratch/rev_fat_alias_base.cb`.

## Repro - no field-to-field store involved at all

```cflat
interface IShape { int area(); };
class Sq : IShape { int s = default; ~Sq() { gfreed = gfreed + 1; } int area() { return 7; } };
struct B { unique IShape t = default; };

extern int main()
{
    B[2] arr = default;
    arr[0].t = new Sq();
    arr[1] = arr[0];        // WHOLE-STRUCT copy - both elements now hold the same box
    printf("area=%d freed=%d\n", arr[1].t.area(), gfreed);
    return 0;
}
```
```
pre a0null=0
area=7 freed=0          compile rc 0, run rc 133 (double free at teardown) - on BOTH binaries
```

A plain `=` between two whole struct VALUES bit-copies the aggregate, including a `unique` field.
Both copies then claim the pointee and both synthesized destructors free it. The `unique` field's
own store paths - the field-to-field implicit move, the reassignment drop-old - are never consulted,
because no field is named on either side.

The pointer-typed `unique T*` field has the same hole; the fat-interface (`unique IShape`) spelling
above is just the one that also defeats the hardening note below.

## Severity

P2. Silent double free (compile 0, abort at teardown, no diagnostic). Pre-existing on the merge
base, so residue rather than a regression - but it is the reachability path for every "two slots
holding the same owner" shape, so it is the one worth closing rather than hardening around.

## Note: the fat-interface drop-old has no same-value skip

`EmitImplicitUniqueFieldMove` (`cflat/MainListener_Declarations.cpp`) releases a fat-interface
field destination's old box before the store. Its safety against a self-assign comes from ORDER -
the source slot is zeroed first, so a genuine self-assign reads a null data pointer and
`DeleteInterfaceValue` no-ops. That covers every case where the two sides are the SAME slot.

It does NOT cover two DIFFERENT slots that happen to hold the SAME box, which is only reachable
once the aliasing hole above has already produced two owners - i.e. the program is already broken
before the store runs (`scratch/rev_fat_alias_eq.cb`, rc 133 on the amended binary; the same file
without the field-to-field line, `rev_fat_alias_base.cb`, is rc 133 too).

Cheap hardening if it is ever wanted independently: compare the loaded old fat value's data pointer
(`extractvalue fat, 1`) against the incoming value's and skip the release when they are equal - the
fat-value twin of the `uq.same` check `EmitUniqueFieldDelete` already performs for thin pointers.
That is a hardening, not a fix: it would keep this store from being the second free without making
the two owners one.

Related: [[implicit-consume-of-a-field-of-a-borrow-local-double-frees]]
