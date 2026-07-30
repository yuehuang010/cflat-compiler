# Generic FUNCTION templates are still bare-keyed, so a namespaced one is silently discarded

Filed 2026-07-29 (round 3 of the key-space work). Found by an adversarial review of
[[generic-template-namespace-key-space]].

Severity: **silent wrong value.** A generic function template declared inside a namespace is
overwritten by (or overwrites) a same-named global one, and calls resolve to whichever survived. No
diagnostic at either declaration. Unchanged by the key-space fix - identical on both binaries.

## Repro

`scratch/rev/p15_genfunc.cb`

```cflat
T ident<T>(T x) { return x; }
namespace NS {
    T ident<T>(T x) { return x + 1; }
    int f() { return ident<int>(10); }     // must call NS.ident -> 11
}
extern int main() { printf("genfunc_ns=%d global=%d\n", NS.f(), ident<int>(10)); return 0; }
```

Correct: `genfunc_ns=11 global=10`. Actual, on the pre-fix binary (`09f1d56`) AND the fixed one:

```
genfunc_ns=10 global=10
```

The namespaced body is simply gone. Declaration order changes the failure mode rather than fixing
it - `scratch/rev/p15b_genfunc_order.cb` declares the namespaced one first and gets a hard error
instead (`no overload of 'ident__int' matches the given arguments`), which is at least loud.

## Root cause

`genericFunctionTemplates` / `genericFunctionTypeParams` / `genericFunctionPackIndex` are keyed on
the bare function name. The only place a namespace reaches that key space is the MEMBER form, which
is keyed `"Owner.method"` (see `GenericMethodOwner` in `MainListener.h`); a free function inside a
namespace contributes its bare name, so two namespaces - or a namespace and global scope - collapse
onto one key and the last registration wins. This is exactly the convention that
[[generic-template-namespace-key-space]] removed for generic STRUCT, CLASS and INTERFACE templates.

Note `LLVMBackend::IsGenericTemplateKey` deliberately does NOT consult
`genericFunctionTemplates`, so the base-name resolution added by that fix does not reach function
templates either. Both halves must move together.

## This falsifies the parent issue's scope claim

[[generic-template-namespace-key-space]] opens with "**every generic template kind** is affected".
Its fix covers struct, class and interface templates; generic FUNCTION templates were never in its
probe table, its 39-leg corpus, or its fix direction. That claim is corrected in that file.

## Fix direction

Mirror what shipped for the other three kinds, in the same order:

1. Key `genericFunctionTemplates` and its sibling maps on the namespace-QUALIFIED name at
   registration, and record the declaring namespace alongside it (do NOT re-derive it from the key -
   struct nesting and namespace nesting share one dotted key space, which is the defect the parent
   issue's round 3 had to fix twice).
2. Add `genericFunctionTemplates` to `LLVMBackend::IsGenericTemplateKey` so a call site's spelled
   name resolves through `ResolveGenericTemplateBase`'s enclosing-namespace walk, innermost first.
3. Keep the existing `"Owner.method"` member-template convention working - `GenericMethodOwner`
   splits on the last dot to recover the owner, so a namespace-qualified free function
   (`NS.ident`) must not be mistaken for a member of a struct called `NS`. That is the same
   ambiguity as item 1 and wants the same recorded-not-derived treatment.
4. `--init`: the mangled instantiation names of namespaced generic functions change, so the cache
   round-trip in `LLVMBackend.cpp` must move in the same change per CLAUDE.md's load-bearing rule.

A duplicate-name diagnostic is the cheaper partial win: even without namespace keying, two generic
function templates collapsing onto one key should not be silent. That overlaps
[[duplicate-generic-template-name-silently-accepted]].

## Test coverage

None. `scratch/rev/p15_genfunc.cb` and `scratch/rev/p15b_genfunc_order.cb` are the repros. A fix
belongs as a positive leg in `Test/test_generics.cb` alongside the `testGnNs*` set.

Related: [[generic-template-namespace-key-space]],
[[duplicate-generic-template-name-silently-accepted]], [[interface-issue-queue]]
