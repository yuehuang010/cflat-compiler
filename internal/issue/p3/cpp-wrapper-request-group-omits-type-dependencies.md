# Refuse a C++ wrapper whose body uses a declaration its request group does not reach (remaining)

## Fixed (CACHE-KEY)

The key defect is closed. Every request group built from a type owner now carries the dependency
groups of the request that registered that type (`cxxTypeOwnerDependencyGroups_`, written by
`NoteCxxTypeOwner`, read by `CollectCxxTypeOwnerGroups`), and the free-call wrapper
(`__cflat_free_*`) collects owners recursively. Measured on master 496b9392e: `b * 4` on
`ckab.Box<ckal.Leaf>` (free operator template in one header, Leaf in another import line) was
keyed on the box header alone; two files pairing that header with two different `ckal::Leaf`
headers shared the key and the second printed garbage (clang 22.00). Legs: "CACHE-KEY" in
Test/test_cpp_interop_template.cb (int Leaf, 57150) and Test/test_cpp_interop_bridge.cb (double
Leaf, 44750). The originally filed `true == bits[0]` wrapper was already keyed sfinae + vector on
496b9392e.

## Still open: the refusal guard

A wrapper can still compile against more than its key names when the incremental TU of its
primary group holds headers an earlier request (or a served replay chunk) added. Nothing refuses
such a wrapper yet. Direction: walk the wrapper body (RecursiveASTVisitor) and refuse a use whose
declaring file `ReachFromRoots` of the request group does not reach (CxxIncrementalGroup has the
include graph). Acceptance: test.sh green with the guard live.

Also unrecorded: `cxxFunctionOwnerGroup_` keeps only the primary group of the request that
registered a free function. Import harvests (no dependency groups) register nearly all of them;
no repro of a function registered under a request with dependency groups.
