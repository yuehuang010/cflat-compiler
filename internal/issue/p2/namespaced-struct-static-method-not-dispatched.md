# A static method on a struct inside a namespace cannot be called at all

Filed 2026-07-30, found while building the corpus for
[[interface-issue-queue]] (landed design records). **Not a generic bug** - it reproduces on a plain
non-generic static method, which is why it is filed separately rather than folded into that issue.

Severity: false rejection, no diagnostic beyond a misleading one. A whole dispatch form is
unavailable.

## Repro

```cflat
import "test_helper.cb";
namespace NSS {
    struct Util { static int plain(int x) { return x + 1; } };
}
extern int main() { printf("static: %d\n", NSS.Util.plain(10)); return 0; }
```

```
v_static.cb(5,52): Undefined variable plain.
```

Both spellings fail: the qualified `NSS.Util.plain(10)` above, and a bare `Util.plain(10)` called
from inside `namespace NSS`.

## What still works

- **Instance** methods on the same shape work fine - only the static form is affected.
- A static method on a struct at GLOBAL scope works.

So the discriminator is precisely "static method + struct inside a namespace".

## Root cause direction

Not investigated. The message is `Undefined variable <method>`, which suggests the call is being
parsed as a member access on a value named `Util` rather than as a static dispatch on the type
`NSS.Util` - i.e. the namespace-qualified TYPE name is not being recognised at the point where
static dispatch is resolved, so it falls through to the variable path. Confirm before acting; this
is an inference from the message, not a diagnosis.

## Relationship to the generic key-space work

None causally - it is not a key-space bug and the generic-template maps are not involved. It matters
here only because it BLOCKS a shape the generic-function corpus wanted to cover: a generic static
member template on a namespaced struct cannot be tested until the non-generic form dispatches.

## Test coverage

None, and no `Test/` file exercises a static method on a namespaced struct today.

Related: [[interface-issue-queue]] (landed design records), [[interface-issue-queue]]
