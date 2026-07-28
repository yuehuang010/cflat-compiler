# Duplicate constructor signature hangs/crashes the compiler with no diagnostic

Found 2026-07-27 during review of `fix/iface-segv` (D2 follow-up). NOT a regression from
that branch - CONFIRMED identical at file scope on master (exit 137, i.e. killed/OOM, in
one run). The `fix/iface-segv` commit newly routes NAMESPACED classes onto this same
already-broken path (previously a namespaced class's explicit ctor crashed earlier, in
the forward-ref scanner, before ever reaching this code).

## Repro

```cflat
namespace ns
{
    class C
    {
        int x = 0;
        C(int a) { x = a; }
        C(int b) { x = b; }
        int g() { return x; }
    };
};

extern int main() { return 0; }
```

```
$ x64/Debug/cflat repro.cb -i Test/library --check
(no output)
```

Symptom is **nondeterministic across runs**: observed exit 137 (killed, likely OOM from
runaway allocation/recursion) in one run and exit 139 (SIGSEGV) in another, both with no
diagnostic printed. The file-scope form (no `namespace ns { ... }` wrapper, same two
`C(int)` overloads) reproduces identically on master, so this is not namespace-specific -
`namespaceName` only changes which struct name is affected, not whether the bug fires.

## Stack trace (one SIGSEGV run, Debug)

```
frame #0-#9: std::map<std::string, LLVMBackend::NamedVariable>::operator[] machinery,
             called with a garbage `this` (map size prints as a huge bogus number) and a
             corrupted key string.
frame #10: LLVMBackend::RegisterThisPointer(...) at LLVMBackend.h:13347
frame #11: MainListener::ParseConstructorDefinition(func, structName="ns.C") at MainListener.h:25771
frame #12: MainListener::ParseClassDefinition(...) at MainListener.h:25514
```

The corrupted map/string state at the crash site is consistent with stack corruption from
unbounded recursion (or a very deep call stack) rather than a clean null-pointer bug like
the ones fixed in this commit - but this has NOT been confirmed. It could equally be an
infinite loop with unbounded heap growth (matching the OOM-flavored 137 exit seen in the
other run). Root cause is UNDIAGNOSED.

## What should happen

Two constructors with an identical parameter list (`C(int a)` and `C(int b)` - the
parameter *names* differ but the signature is the same `C(int)`) should be rejected with a
clean `LogError` naming the duplicate signature, the same way other duplicate-declaration
cases are diagnosed elsewhere in the compiler. No duplicate-constructor-signature
diagnostic currently exists anywhere in the codebase (grepped for "duplicate" +
"constructor" / "ctor" in `LLVMBackend.h` / `MainListener.h` - no hits).

## Fix direction

1. First confirm whether this is recursion (find the cycle - likely somewhere in overload
   resolution or `CreateFunctionDeclaration` re-entering itself when it finds an existing
   symbol with the same mangled name) or a loop with no bounded exit condition. A debug
   build with a lowered stack-overflow guard, or instrumenting `RegisterThisPointer` /
   `ParseConstructorDefinition` entry, should narrow it quickly.
2. Once the mechanism is known, add an eager duplicate-signature check when registering a
   second constructor with the same parameter type list (ignore parameter names, per C
   semantics - same convention used for ordinary overload duplicate checks, if one
   exists), with a `LogError` naming both source locations if possible.
3. Add a regression test to `Test/errors/err_*.cb` once fixed.

## Out of scope note

A related-looking but DIFFERENT pre-existing failure - `ns.Box<int>(5)` failing with
`unknown function 'ns.Box'` on both the pre-fix and post-fix binaries - is NOT this bug.
That one is the generic-class early-return path (`ScanStructOrClassDefinition`'s
`ctx->genericTypeParameters() != nullptr` branch) never registering an instantiated
generic's namespaced constructor name; it is a separate, already-known-shaped gap and is
left untouched here.
