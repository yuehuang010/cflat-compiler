# A namespace alias resolves structs but not generic template bases

Filed 2026-09-03 from the review of the scoped-registry refactor (q02 member 4, c6f23a3).
Pre-existing: identical on master 4bfa4aa and on the refactor branch.

## Summary

`using IN = Outer.Inner;` lets `IN.Box2` name the plain struct `Outer.Inner.Box2`, but
`IN.GBox<int>` fails with `cannot find the type 'IN.GBox<int>'`. The alias hop is applied when a
dotted spelling names a struct or interface and skipped when it names a generic template base.

## Repro

`scratch/q02m4_ns_alias_generic_base.cb`:

    namespace Outer { namespace Inner { struct GBox<T> { T v = default; }; } }
    using IN2 = Outer.Inner;
    namespace Other2
    {
        int use() { IN2.GBox<int> b; b.v = 8; return b.v; }   // (5,25): cannot find the type 'IN2.GBox<int>'
    }

Same file with a non-generic `struct Box2` and `IN2.Box2 b;` compiles and runs.

## Root cause

`LLVMBackend::ResolveGenericTemplateBase` / `ResolveTypeArgBaseName` (cflat/LLVMBackend_Interfaces.cpp)
call `ScopedNameCandidates` with `ResolveFirstComponentAlias = false` (the refactor named the
policy that the old hand-rolled walk implemented): template keys are registered verbatim at the
declaration site, and the generic path never hopped the first component through
`namespaceAliasTable`. `ResolveQualifiedName` for structs does hop.

## Fix direction

Turn `ResolveFirstComponentAlias` on for the generic-base lookups, then confirm the instantiation
key that gets queued is the canonical `Outer.Inner.GBox<int>` (not `IN2.GBox<int>`) in both
pre-scans (`ForwardRefScanner::ScanGenericTypeUses`, `MainListener::ScanAndQueueGenericTypeUses`)
so the two walks and codegen agree; add a leg to `testNamespaceTypeShadowing` in
`Test/test_basic.cb`. Check the mangled name goes through the alias too (`--symbol-dump`).
