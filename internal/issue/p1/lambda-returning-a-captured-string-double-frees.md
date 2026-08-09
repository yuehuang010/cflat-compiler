# Returning a CAPTURED `string` from a lambda double-frees it at teardown

Filed 2026-08-09 by the review of `fix/lamtemp`, while building the must-still-work accept set
for the expression-body lambda's newly-active return gates. **Pre-existing** - measured
identical on master `0669ebc` and on `fix/lamtemp`, in BOTH the expression-body and block-body
spellings, so it is not that fix's doing and not a divergence between the two spellings.

Severity: **P1, memory unsafety.** The program prints the right values and then aborts with
rc 133 (SIGTRAP out of the allocator) with no message. Reachable in one line of ordinary source.

## Repro

```cflat
extern int main()
{
    string s = "xyz";
    Lambda<string()> g = () => s;
    string r = g();
    printf("r=%s s=%s\n", r.data(), s.data());
    return 0;
}
```

```
r=xyz s=xyz
```
compile rc 0, run **rc 133**. The block-body twin `() => { return s; }` behaves identically
(compile rc 0, prints `xyz`, rc 133), on both binaries.

## The cells, measured on both binaries (warm `--init-local`)

| spelling                                                  | result   |
|-----------------------------------------------------------|----------|
| `Lambda<string()> g = () => s;` (captured local)           | rc 133   |
| `Lambda<string()> g = () => { return s; };`                | rc 133   |
| `Lambda<string()> g = () => s.copy();`                     | rc 0     |
| `Lambda<string(string)> g = (string t) => t;` (parameter)  | rc 0     |
| `Lambda<int()> g = () => s.length();` (captured, not returned) | rc 0 |
| free function `string f() { return gs; }` over a global    | rc 0     |

So the discriminator is precisely **returning the captured string by value**: the returned
`string` and the closure environment's captured copy end up owning one buffer, and both
destructors run.

## Why the return gates do not catch it

The struct twin IS caught. With an owning CLASS instead of a `string`:

```cflat
class OwnS { string s = default; int v = default; ~OwnS() { } };
OwnS a = default;
Lambda<OwnS()> f = () => a;
```

both spellings are rejected on both binaries with
`cannot return an 'alias' value 'a'; it borrows storage it does not own and would dangle.`
That is `SourceIsDanglingAliasBorrow`, and per the landed record it is `IsAlias || IsAliasBorrow`
**minus `string` / closure / POD**. The `string` carve-out is what lets this cell through.
(On master the class cell was worse in the expression body - it compiled and aborted rc 133 -
and `fix/lamtemp` converged it onto the rejection; the `string` cell is the residue that carve-out
leaves behind in BOTH spellings.)

## Fix direction

Do NOT simply delete the `string` exclusion from `SourceIsDanglingAliasBorrow` - it exists
because ordinary `return someString;` out of a named function is correct and extremely common
(`c2c` above), and the standing rule for this predicate family is prove-what-you-reject. The
discriminating fact here is narrower: the source is a **by-value capture living in the closure
environment**, whose copy is destroyed when the environment is, so handing the same buffer to
the caller creates a second owner.

Two candidate shapes, in preference order:

1. Make the lambda's by-value string capture a real owning COPY at capture time and let the
   return copy from it (i.e. fix the ownership of the capture, not the return) - then both
   owners are genuine and nothing is shared.
2. Failing that, ask the narrow question at the return site: the returned `string` resolves to a
   capture slot of the enclosing closure environment. Reject with the `.copy()` remedy, which is
   measured working (`() => s.copy()`, rc 0).

Check the neighbouring owning value types before scoping - `list<T>` and `dictionary<K,V>`
captured and returned the same way are the obvious siblings and were not measured here.

Related: [[alias-borrow-remaining-launder-sites]], [[discarded-copy-result-in-a-call-chain-leaks]].
