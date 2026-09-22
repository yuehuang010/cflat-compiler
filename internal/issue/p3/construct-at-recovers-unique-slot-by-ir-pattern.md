Bucket: p3 (code quality; no wrong behaviour measured)

# construct_at recovers a unique<T> element slot by pattern-matching the LLVM GEP

Found 2026-09-21 in main-session review of 497d4fae (construct_at builtin).

`&_data[i]` on a `unique<T>` element unwraps the holder to its inner `_p` pointer (the general
address-of rule for unique holders). `construct_at(&_data[i], move v)` needs the HOLDER slot, so
the builtin (cflat/MainListener_PostfixExpression.cpp, the `functionName == "construct_at"` arm)
dyn_casts the slot value to a GetElementPtrInst, reads the source element struct NAME, and when it
is a core unique type steps back to the GEP's pointer operand.

Why it matters: the type decision is driven by IR shape, not by the type model. Any change to how
the unwrap is emitted (a different GEP form, an intervening cast, constant folding) silently turns
the construct into a store through `_p`. Balanced counts in Test/test_generics.cb pin today's
behaviour only.

Fix direction: parse the first argument of construct_at through an address-of path that does NOT
unwrap a unique holder (a flag on the unary '&' handler, the way declExpectedType scopes other
context), and delete the GEP sniffing. Acceptance: list<struct with unique<int>> add/insert/remove
counters unchanged; the GetElementPtrInst dyn_cast is gone.
