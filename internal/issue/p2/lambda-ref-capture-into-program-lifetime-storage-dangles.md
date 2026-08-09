# A lambda that ref-captures a local can be stored in program-lifetime storage

Filed 2026-08-09 by the review of the fix that gave `static` locals their own storage. The
GLOBAL half is **pre-existing on master**; the fix adds the `static` local as a second
spelling of the same hole. Deliberately not fixed there: closing it needs escape analysis
through captures, which that fix explicitly did not attempt (it rejects only the directly
enumerable `&local` / view-of-a-local shapes).

Severity: **P2, memory unsafety** - reads a dead frame; the value it returns is whatever
later reuses that stack slot.

## Repro A - file-scope global (master and branch alike, no diagnostic)

```cflat
struct Box { int n = default; };
Lambda<int()> gl = default;
void f(int seed)
{
    Box b = Box();
    b.n = seed;
    gl = () => { return b.n; };   // captures `b` by reference into a global
}
extern int main() { f(11); printf("after return %d\n", gl()); return 0; }
```

Accepted with no diagnostic on master (`324d780`) and on the branch. Both print
`after return 11` here - the dead frame is still intact, which is the usual way this
defect hides.

## Repro B - static local (branch only)

```cflat
struct Box { int n = default; };
Lambda<int()> gl = default;
int f(int seed)
{
    Box b = Box();
    b.n = seed;
    static Lambda<int()> L = () => { return b.n; };
    gl = L;
    return L();
}
extern int main()
{
    printf("inside %d\n", f(11));
    printf("after return %d\n", gl());
    return 0;
}
```

Prints `inside 11`, then `after return 124666700` - a plain use-after-return through the
captured address. Calling `L` again from INSIDE a later `f` reads whatever the current
frame's `b` holds (measured `11` then `22`), which is address reuse, not a live capture:
`L` is initialized once, so the second call cannot have re-captured.

## Root cause (hypothesis, unverified)

A by-reference capture stores the address of the captured local's alloca in the closure
env. The bond/borrow checker validates capture lifetimes against the CAPTURING scope, not
against the storage duration of whatever the closure value is finally stored into, so a
store of the closure into a global (or now a `static` local) is unexamined.

## Fix direction

At the store into program-lifetime storage - a file-scope global, or a `NamedVariable`
with `IsStaticLocal` - reject a closure value whose env holds any by-reference capture of
a non-static local. The capture list is already known at the lambda literal
(`NamedVariable::LambdaCaptureNames`); what is missing is the by-reference-vs-by-value
distinction surviving to the store site. A by-VALUE capture is safe and must stay accepted
(that is the common spelling), so the check has to be capture-kind-aware, not
capture-count-aware.
