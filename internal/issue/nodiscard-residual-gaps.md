# nodiscard: residual known gaps (value-identity detection)

The mandatory-nodiscard check (a discarded owning RETURN value is a compile error) is
implemented by value identity: every owning-return call result is ledgered
(`ownedReturnTemps_` in `LLVMBackend.h`), and at a discard position (bare expression
statement, for-init, for-update) `DiagnoseDiscardedOwningReturn` (`MainListener.h`) fires
only when the full expression's RESULT value is a still-unconsumed owning return. That design
leaves one hole by construction (Gap 1 below). It is bounded and is not a regression introduced
by the feature. Do not fix as part of the nodiscard work.

(Gap 2 - a buried owning temp consumed by an enclosing expression, e.g.
`if (makePtr() != nullptr) { }` - was a real LEAK and has been FIXED: an owning-POINTER call
result consumed as a comparison operand or as a scalar-field deref base is now registered in
`pendingOwnedPtrTemps` and freed at end-of-full-expression, the pointer analog of the existing
`pendingOwnedStringTemps` / `pendingOwnedStructTemps` machinery. Both sites are ones the pointer
provably cannot escape from - a comparison yields a bool, a scalar field read copies a
self-contained value.

An owning-POINTER temp passed as a CALL ARGUMENT still leaks and is deliberately left alone: a
borrow parameter may legally RETAIN its argument (store it in a global / a field / return it),
which CFlat does not diagnose, so freeing it in the caller is a use-after-free rather than a leak
fix. Deciding it needs interprocedural escape analysis. `Test/test_collection_leaks.cb` pins the
retaining-callee shape as a positive test so this is not silently "fixed" later.)

## Gap 1 - indirect (closure / function<> / fnptr) owning return, bare-discarded

`h();` where `h` is a `function<move R*>` (or a closure / raw fn pointer) whose return is
owned is NOT ledgered - only the two named-call paths (direct
`CreateOverloadedFunctionCall` and interface/virtual dispatch) register results. So the
policy error is not raised.

- Harm: error-MISSING, NOT a leak. The value is still freed (`dtor == 1`) by the ordinary
  owned-temp machinery; only the nodiscard diagnostic is absent.
- Fix direction (if ever wanted): ledger owning returns at the indirect-call emission site
  too, keyed by the resolved callee's return-ownership.

## Note - IsUniqueTypeArg interface-return branch is defensive/untested

`LLVMBackend.h` (interface/virtual dispatch, the `(rt.IsMove || rt.IsUniqueTypeArg) && ...`
line) registers an owning return for a substituted `unique X*` type-arg returned THROUGH an
interface method. This branch is currently gated unreachable by the interface
return-declaration contract rule (a conforming class cannot declare the matching `move`
return without tripping the contract check first), so no test exercises it. It is kept as
defensive code mirroring the direct-call path. The reachable, tested path is the plain
`move` pointer return via the direct call path.
