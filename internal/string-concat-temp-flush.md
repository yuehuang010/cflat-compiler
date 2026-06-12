# Owned-string temporary cleanup (chained-concat leak fix)

How the chained-concat intermediate leak fix tracks and frees unnamed owned-string
temporaries. Read before touching `TryBinaryOperatorOverload`, the ownership
propagation in `ParseDeclaration` / `ParseAssignmentExpression`, the `move`-param
handling in `CreateOverloadedFunctionCall`, or `FlushOwnedStringTemps`.

Bug background: chained concat `a + b + c` leaked the unnamed `(a + b)` intermediate -
`operator+` returns an owned string, but a result consumed as a sub-expression never
landed in a `NamedVariable`, so the scope-destructor pass never freed it. Detector:
`HeapAudit.reportLeaks()` (`core/diagnostic/heap_audit.c`/`.cb`) scans the audit table
for still-live allocations. Regression cases: `Test/test_core.cb::testStringAPI()`.

## The model

`operator+` on `string` is `ReturnsOwned`: its result owns a fresh heap buffer. In a
chain `a + b + c`, the `(a + b)` result is an unnamed SSA temporary consumed as the next
`+`'s left operand - it never lands in a `NamedVariable`, so the scope-destructor pass
never frees it. The fix mirrors C++ temporary lifetime: free such temporaries at the end
of the full expression (the statement's semicolon).

`LLVMBackend::pendingOwnedStringTemps` is a `vector<{llvm::Value*, llvm::BasicBlock*}>`:
the string SSA value plus the block it was created in.

- **Register**: `TrackOwnedStringOperatorResult` (MainListener) pushes every owned-string
  operator result. Called from *both* return paths of `TryBinaryOperatorOverload`, AND
  from the `char* + char*` concat branch in `ParseAdditiveExpression` (which dispatches
  `operator+(const char*, const char*)` manually because `TryBinaryOperatorOverload`
  only fires off a struct lvalue - both operands there are raw i8*). Any future site
  that calls an owned-string-returning operator outside `TryBinaryOperatorOverload`
  must register its result the same way or the temp leaks.
- **Unregister** (whoever takes ownership, to avoid a double free):
  - named-local init (`ParseDeclaration`, `IsOwningString` branch);
  - plain `=` assignment (`ParseAssignmentExpression`, guard `operatorText == "="` - a
    compound `+=` RHS is a true intermediate and must stay queued);
  - `move string` parameter (`CreateOverloadedFunctionCall` move loop, by
    `matched[i].Primary`);
  - `return` (removes the returned value before flushing).
- **Flush**: `FlushOwnedStringTemps` at each block-item boundary (`ParseBlockItemList`)
  and before `CreateReturnCall`. Spills each temp to an entry-block alloca and calls the
  string dtor.

## Traps

- **Double free** is the whole risk. Every owner path MUST unregister the value it claims.
  When adding a new sink for an owned string (a new container method, a new statement form
  that stores a string by value), unregister the moved/stored SSA value or the flush will
  free a buffer the sink now owns. The `--asan` run of `Test/test_core.cb` is the guard.
- **Value identity**: removal is by `llvm::Value*` pointer identity. The registered value
  is exactly what `TryBinaryOperatorOverload` returned and flows unchanged through
  `LoadNamedVariable` for a value-only struct NV, so `right` / `matched[i].Primary` match
  it. If you insert a cast/copy between the operator result and the binding site, identity
  breaks and the temp leaks (not a double free - safe, just a missed free).
- **Dominance / same-block only**: `FlushOwnedStringTemps` frees only temps whose creating
  block == the current insert block, and is a no-op if that block is already terminated.
  A temp created across a branch (concat in a loop/if *condition*) would not dominate the
  flush point; emitting a free there is invalid IR, so it is dropped (documented leak)
  rather than freed. The list is always cleared at each flush so nothing carries across
  statements. `pendingOwnedStringTemps` is also cleared in `ResetForReanalysis`.
- **Only operator results**: a bare `move string`-returning *function* call consumed as a
  sub-expression (not via `operator+`) is the same leak class but is not tracked - it was
  already leaking and is out of scope here.
