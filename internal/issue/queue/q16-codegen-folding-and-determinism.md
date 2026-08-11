# q16: Codegen - constant folding, determinism, emission-order coupling

4 items. Emitted IR depends on the ORDER in which things happen to be emitted, or on an arbitrary
budget, rather than only on the source.

## Shared root cause

Emission-order coupling. The global-constant fold requires the constructor to already exist as an
`llvm::Function` and gives up past a fixed recursion depth; switch-case collection iterates an
unordered, pointer-keyed container; and nested emission clears per-function state that the
`BuilderState` save/restore does not cover. All four produce output that varies with something the
user did not write.

## Members

- `p2/global-fold-depth-cap-five-levels` - fixed recursion budget on the default-construction
  fold; a non-constant struct field breaks eager `insertvalue` folding.
- `p3/global-fold-value-depends-on-instantiation-order` - the fold requires the constructor to be
  emitted already; emission order varies.
- `p3/nondeterministic-ir-switch-case-order` - unordered/pointer-keyed iteration (UNCONFIRMED -
  confirm by diffing `--out-lli` across runs before writing a fix).
- `p2/nested-emission-clears-enclosing-alias-scope-registry` - `createFunctionBlock` clears the
  per-function noalias registry; `BuilderState` save/restore does not cover it.

## Fix direction

1. Make the fold independent of emission order: either force constructor emission on demand from
   the fold, or defer the fold to a post-emission pass. The depth cap should then be removable, or
   at minimum should emit a diagnostic instead of silently producing a different value.
2. Key the switch-case collection on a deterministic ordering (source order or a stable name), then
   verify with a byte-identical `--out-lli` across two runs.
3. Add the alias-scope registry to `BuilderState` save/restore so nested emission cannot clear an
   enclosing function's state.

Determinism is cheap to VERIFY (compile twice, diff the `.ll`), so add that check to the fix rather
than reasoning about it. Disjoint from ownership and from the parser buckets.
