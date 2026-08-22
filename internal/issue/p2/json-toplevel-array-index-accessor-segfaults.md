# By-index accessors on a TOP-LEVEL JSON array segfault

Found 2026-08-21 while adding the wide-number regression legs for
[[json-numbers-stored-as-32-bit-float]]. Pre-existing: reproduces identically on the p2-bundle
binary and on the main checkout's `master` binary, so it is not a regression from that work.

## Repro

```cflat
import "json.cb";
extern int main() {
    JsonNode* doc = JsonParser.parse("[1,2,3]");
    printf("n=%d\n", doc.getInt(0));   // SIGSEGV
    delete doc;
    return 0;
}
```

`exit=139` (SIGSEGV), no diagnostic. The same document read through a nested array
(`JsonParser.parse("{\"xs\": [1,2,3]}")` then `doc.getArray("xs").getInt64(0)`) works correctly -
that is the shape `Test/test_reflect.cb` uses, with a comment pointing here.

## Suspected root cause (not yet confirmed with a debugger)

`JsonNode._getIndex` (`cflat/core/json.cb:269`) walks `firstChild` / `child.next`. For a top-level
array parsed by the static `JsonParser.parse`, the node's child chain appears not to be populated
the way the nested-array path populates it - note the comment at `cflat/core/json.cb:406` that the
static `parse()` path heap-allocates nodes with a null `_arena`, i.e. it is a second, less-used
construction path. The crash is a null/garbage dereference inside the walk or in the caller that
dereferences the returned node without a null check (`getInt64(int)` at line 353 does
`JsonNode* n = _getIndex(index);` and the callers at 365/376/386/394 follow the same shape).

## Fix direction

1. Confirm with a debug build whether `firstChild` is garbage or whether the null return from
   `_getIndex` is dereferenced unchecked. Fix the actual cause; do not paper over it with a null
   guard alone.
2. Whichever it is, the by-index accessors must return the documented "0 if absent" rather than
   crash, so add the missing null checks in the same change.
3. Regression leg: extend the by-index block in `Test/test_reflect.cb` with a top-level array
   (`JsonParser.parse("[394328000000, 2.01]")`) asserting `getInt64(0)` / `getDouble(1)` by value,
   and drop the "crashes on master too" comment there.
