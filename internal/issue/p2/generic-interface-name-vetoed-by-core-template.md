# A core generic struct/class template vetoes a same-named user generic interface

Re-filed 2026-07-29 (round 2 of the key-space fix). This shape was folded into
[[interface-issue-queue]] (landed design records) as its step 4, then implemented and **reverted**; that
issue's other four steps shipped without it. It is back on its own because it is NOT a key-space
problem and cannot be solved by qualifying namespaces.

Severity: **false rejection, no miscompile.** Master-parity - the name simply keeps its pre-existing
behaviour. What makes it worth fixing is reach, not danger.

## The bug

`LLVMBackend::IsGenericInterfaceTemplateName` resolves a generic-interface / generic-struct name
collision in favour of the struct, globally and per bare name:

```cpp
if (gts.genericStructTemplates.count(name) != 0
    || gts.genericClassTemplates.count(name) != 0
    || gts.scannedGenericStructNames.count(name) != 0)
    return false;
```

The tie-break is *required* - `Test/test_generics.cb` declares `Container<T>` as both a generic
struct (line 21) and a generic interface (line 204) and needs `Container__int` to be a real struct
type - but a CORE template is on every compile's import graph, so it vetoes an unrelated user
interface of the same name:

```cflat
import "list.cb";
interface list<T> { T Get(); };            // 'list' is a generic CLASS in core/list.cb
class L<T> : list<T> { T d = default; T Get() { return d; } };
int use(list<int> l) { return l.Get(); }   // Unknown identifier 'Get'.
```

Repro: `scratch/nsgi/leg_veto_list.cb` (corpus leg 25 of the key-space issue, removed from its
runnable set) -> `leg_veto_list.cb(5,35): Unknown identifier 'Get'.`

Reachable names, from a sweep of `cflat/core/`: `block_pool`, `arena_channel`, `array`, `channel`,
`dictionary`, `HResult`, `ComPtr`, `hashset`, `list`, `page_arena`, `Pair`, `queue`, `span`, `stack`,
`spsc_queue`, `TaskResult`, `tuple`, `view`. Of these `Pair`, `array`, `list`, `span`, `queue`,
`stack` and `view` are plausible user interface names, so the set is not theoretical.

## Two remedies already tried and rejected - do not retry these blind

### 1. Key the tie-break on module INEQUALITY (interface wins when the roles come from different files)

Contradicted by a ratified test. `Test/test_interface.cb` legs 16/17/19 - "Cross-file bare-name
collisions - the struct role owns the mangled name" - pin a user `interface GiCollideRev<T>` LOSING
to a `struct GiCollideRev<T>` imported from `Test/library/gi_collide_struct.cb`. That is the SAME
shape as the `list` case with the opposite expected outcome. Implementing it produced:

```
test_interface.cb(3733,4): 'v' does not name a value here. If it is a method, call it: 'v()'.
Darwin (Release): 529 passed, 1 failed, 8 skipped
```

### 2. Key it on CORE-vs-USER (interface wins only when the sole struct claimant is a core template)

Keeps `./test.sh Release` green at 530/0/8 and makes the repro above pass - and is still
unshippable, because it **trades one false rejection for another, on the more common shape**.
Declaring `interface list<T>` makes core's own `list<T>` container unusable in that program:

`scratch/nsgi/t3_clash.cb`

```cflat
import "test_helper.cb";
import "list.cb";
interface list<T> { T Get(); };
class L<T> : list<T> { T d = default; T Get() { return d; } };
extern int main()
{
    list<int> c = default;
    c.add(4);
    printf("core count=%d\n", c.count());
    return 0;
}
```

- pre-fix binary (`09f1d56`): `core count=1` - compiles and runs.
- with the core-vs-user tie-break: `t3_clash.cb(8,4): no overload of 'add' matches the given
  arguments.` (the candidate dump then lists `list__string` and `atomic_counter` overloads, because
  `list__int` is now a fat pointer). A REGRESSION, not a tightening.

Using core `list` is far more common than declaring your own `interface list<T>`, so this direction
is strictly worse than the status quo.

Also do NOT key it on whether the core template "happens to be used" in the program: that is
order-dependent and does not survive the `--init` warm cache.

## Why no tie-break can work - the actual obstruction

The two shapes are **mutually exclusive**, and this is what the superseded framing missed. Both
spell a bare `list<int>` at GLOBAL scope:

- `scratch/nsgi/leg_veto_list.cb` needs `list<int>` to mean the user INTERFACE.
- `scratch/nsgi/t3_clash.cb` needs `list<int>` to mean core's CLASS.

Same spelling, same scope, opposite required meaning. Any deterministic rule satisfies one and
breaks the other. `global::` - the only scope-escape qualifier in the grammar
(`Identifier DoubleColon genericIdentifier`) - cannot help either, because both roles are already at
root scope.

Nor does giving the two roles distinct MANGLED names ("make both roles coexist") solve it: that
removes the symbol clash but leaves the use-site spelling ambiguous, which is the part that
actually decides the program's meaning. `t3_clash.cb` needs both meanings live in one file.

## Fix direction

This is a language-design call, not a tie-break tweak. Pick one:

1. **Reject the collision.** A generic interface and a generic struct/class template that share a
   name and a scope become a compile error at the second declaration. Cleanest and gives the user a
   diagnostic instead of a mystery `Unknown identifier`. Blocked by `Test/test_generics.cb`, which
   *depends* on the same-scope `Container<T>` collision - so this needs that test's legs 21/204
   renamed first, which is a maintainer decision. Overlaps
   [[duplicate-generic-template-name-silently-accepted]], whose stated blocker is the same test.
2. **Add a disambiguating spelling** so a contested name can be resolved explicitly at the use site
   (e.g. an `interface`/`struct` role qualifier, or letting `global::` carry a role). Bigger, and
   only worth it if collisions are meant to be legal.
3. **Leave it.** Document the reserved-name list above so a user hitting it can rename. The current
   behaviour is at least consistent (struct always wins) and never miscompiles.

Whichever is chosen, the diagnostic is the immediate win: today the user gets
`Unknown identifier 'Get'` pointing at their own method call, with nothing naming the collision.

## Test coverage

None in `Test/`. `scratch/nsgi/leg_veto_list.cb` (fails) and `scratch/nsgi/t3_clash.cb` (passes,
must keep passing) are the two shapes any fix must satisfy simultaneously - which is the point.
`Test/test_interface.cb` legs 16/17/19 and `Test/test_generics.cb`'s `Container<T>` are the existing
constraints. Corpus leg 39 (`testVetoListControl`, `interface myList<T>` with no collision) passes
and stays in the key-space corpus as the negative control.

Related: [[interface-issue-queue]] (landed design records),
[[duplicate-generic-template-name-silently-accepted]], [[interface-issue-queue]]
