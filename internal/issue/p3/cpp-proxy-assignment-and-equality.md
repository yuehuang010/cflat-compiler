Bucket: **p3** (usable-surface gap on a C++ proxy class; no wrong values).

# A C++ proxy returned by `operator[]` cannot be assigned to, and does not compare

Follow-ups split off the vbool-proxy conversion fix (commit on fix/cpp-vbool-proxy). That change
made `std::vector<bool>`'s `std::__bit_reference` proxy CONVERT at every implicit-conversion site.
Two neighbouring shapes stay refused, each for a different root cause, and each needs its own
accept-set.

## 1. `v[0] = true` - assignment INTO the proxy

```cflat
std.vector<bool> v = default;
v.push_back(false);
v[0] = true;    // vbp_a12_proxyassign.cb(6,4): Left side of assignment is not an addressable lvalue.
```

C++ resolves this to `__bit_reference::operator=(bool)`. CFlat refuses before any operator lookup
because the left side is a call RESULT, not an addressable lvalue. Root cause is the lvalue-ness
rule for a call result, not conversion lookup - a different site from the landed fix.

Accept-set question a ruling must settle: which call results become assignable? Only ones whose
class declares `operator=`? Only C++ records? Does the same apply to a CFlat struct returned by
value from a member?

## 2. `v[0] == true` - the proxy as a binary-operator operand

```cflat
if (v[0] == true) { }   // no operator '==' for type 'std.__bit_reference<std.vector<bool, std.allocator<bool>>, true>'
```

C++ converts the proxy to `bool` through `operator bool` and uses the builtin `==`. CFlat's binary
operator path never applies a user-defined conversion to an operand. Root cause is operand
conversion in TryBinaryOperatorOverload, a third site.

Accept-set question: applying a user conversion to an operand has to be bounded (which operators,
one operand or both, how ambiguity between two conversion targets is resolved) - that is a ruling,
not a mechanical extension.

## Repro

`scratch/vbp_a12_proxyassign.cb` and `scratch/vbp_a11_eqtrue.cb` in the fix/cpp-vbool-proxy
worktree, both measured refused before and after that commit.
