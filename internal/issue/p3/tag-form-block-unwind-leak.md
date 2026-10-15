# Tag-form heap object leaks its block when an attribute throws

`T261Pair* p = <T261Pair a={h0} b={mk(x)} />;` where `mk` throws: the fields built before the throw are
now destroyed (T261), but the `operator new` block from EmitHeapDefaultConstruct is never freed on the
unwind path (no UnwindPartialEntry for the block). Reproduced with `--heap-audit`: 1 LEAK on master and
after the fix. Memory only; no object is destroyed twice.

Fix direction: note the block in ParseElementExpression (ParseElementExpression in
MainListener_PostfixExpression.cpp) with the matching free. CflatHeap frees through the default
operator delete, which is wrong when the element tag resolves to a user `operator new`, so the free
must pair with the allocator actually used.
