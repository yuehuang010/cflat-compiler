# nodiscard: residual known gaps (value-identity detection)

The mandatory-nodiscard check (a discarded owning RETURN value is a compile error) is
implemented by value identity: every owning-return call result is ledgered
(`ownedReturnTemps_` in `LLVMBackend.h`), and at a discard position (bare expression
statement, for-init, for-update) `DiagnoseDiscardedOwningReturn` (`MainListener.h`) fires
only when the full expression's RESULT value is a still-unconsumed owning return. That design
leaves two holes by construction. Both are bounded; neither is a regression introduced by the
feature. Do not fix as part of the nodiscard work.

## Gap 1 - indirect (closure / function<> / fnptr) owning return, bare-discarded

`h();` where `h` is a `function<move R*>` (or a closure / raw fn pointer) whose return is
owned is NOT ledgered - only the two named-call paths (direct
`CreateOverloadedFunctionCall` and interface/virtual dispatch) register results. So the
policy error is not raised.

- Harm: error-MISSING, NOT a leak. The value is still freed (`dtor == 1`) by the ordinary
  owned-temp machinery; only the nodiscard diagnostic is absent.
- Fix direction (if ever wanted): ledger owning returns at the indirect-call emission site
  too, keyed by the resolved callee's return-ownership.

## Gap 2 - buried owning temp consumed by an enclosing expression

`if (makePtr() != nullptr) { }` (and any owning call used only as an operand /
subexpression of a discarded statement or a condition) leaks (`dtor == 0`): the comparison
CONSUMES the owning temp as an operand, so nodiscard - which only inspects the full
expression's RESULT value - cannot and must not see it.

- This is the broad PRE-EXISTING "buried owning temp in a subexpression" cleanup gap, not a
  nodiscard concern. It references the general owned-temp-in-subexpression cleanup work
  (the `pendingOwnedStringTemps` / `pendingOwnedStructTemps` flush machinery already handles
  strings/structs used as operands; the owned-POINTER analog is the missing piece).
- nodiscard deliberately does not flag it: the value is consumed by the comparison, so it is
  not the discarded result.

## Note - IsUniqueTypeArg interface-return branch is defensive/untested

`LLVMBackend.h` (interface/virtual dispatch, the `(rt.IsMove || rt.IsUniqueTypeArg) && ...`
line) registers an owning return for a substituted `unique X*` type-arg returned THROUGH an
interface method. This branch is currently gated unreachable by the interface
return-declaration contract rule (a conforming class cannot declare the matching `move`
return without tripping the contract check first), so no test exercises it. It is kept as
defensive code mirroring the direct-call path. The reachable, tested path is the plain
`move` pointer return via the direct call path.
