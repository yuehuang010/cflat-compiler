# Returning a nontrivial C++ class BY VALUE from a CFlat function skips the C++ copy constructor

## Summary

A CFlat function whose return type is a nontrivial foreign C++ class returns the class in
REGISTERS and copies it with the synthesized CFlat struct copy helper
(`<type>.copy.synth`), not with the class's C++ copy constructor. Returning a field is
therefore a silent extra owner: the caller's value and the source both release the same
resource. Found 2026-09-16 while fixing the field-lvalue initializer defect
(`cpp-shared-ptr-vector-stale-stack-control-block.md`); measured, not hypothesized.

## Repro

```cflat
import cpp "cpp_interop_tpl.h";

struct Holder { std.shared_ptr<cppt.ModuleBase> p = default; };

std.shared_ptr<cppt.ModuleBase> ret_field(Holder* h) { return h->p; }

extern int main()
{
    Holder h = default; h.p = std.make_shared<cppt.ModuleBase>();
    { std.shared_ptr<cppt.ModuleBase> r = ret_field(&h);
      printf("uc=%d want 2\n", (int)r.use_count()); }   // measured 1
    printf("after uc=%d want 1\n", (int)h.p.use_count());  // measured 5 (freed)
    return 0;
}
```

`scratch/byval_c16.cb` in the byval worktree is this program. Returning a plain LOCAL
(`scratch/byval_c17.cb`) is correct today, because the local's copy happened at its
declaration and the return then moves it.

## Root cause (measured)

`--symbol-dump-ir function:ret_field` shows:

```
%2 = load %"std.shared_ptr$cppt.ModuleBase", ptr %1
%3 = call %"std.shared_ptr$cppt.ModuleBase" @"std.shared_ptr$cppt.ModuleBase.copy.synth"(...)
ret %"std.shared_ptr$cppt.ModuleBase" %3
```

Two separate problems: the return is not sret (the Itanium ABI requires indirect return for a
nontrivial class), and the copy is the field-wise synth helper instead of the C++ copy
constructor.

## Fix direction

Give a CFlat function that returns a nontrivial foreign C++ class an sret parameter, and
copy- or move-CONSTRUCT the returned lvalue into it. The declaration side already does this
(`TryDeclareForeignCxxLocal` -> `EmitCxxCopyOrMoveConstruct`); the return statement in
`MainListener_Statements.cpp` has no C++-class arm at all.

Acceptance: the repro prints `uc=2` then `after uc=1`, and a fixture leg asserts both.

## Related symptom (same root, found in round-1 review)

`MainListener::RegisterOwningTempReceiver` (cflat/MainListener_PostfixExpression.cpp ~8600)
bitwise-stores a receiver into a `recvtemp` alloca and then `RegisterOwnedStructTemp`s it, so a
destructor runs on a copy that never ran a copy constructor. Every earlier arm returns first for
an lvalue receiver (`receiver.Storage != nullptr`) and for an sret return temporary, so the store
is only reachable for a nontrivial C++ class that arrived in REGISTERS - i.e. only through the
register-return lowering above. Fixing the sret return removes this site's only input; re-check
it when doing so.
