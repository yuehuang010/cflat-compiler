# q03 - Standalone correctness fixes, no shared root

Three independent one-area items (HeapAudit JIT gap ruled no-fix 2026-09-03: use `--heap-audit` AOT; ternary join added from deferred). Grouped for scheduling only; each can run alone and in parallel
with q02 (disjoint files).

## Members

| Item | Status | Shape |
|------|--------|-------|
| `p3/narrow-integer-promotion-gaps` | LANDED c56efdf | unary `-`/`~` on narrow operands promote to signed int; mixed-sign narrow ternary arms join signed; return into a narrow declared type truncates like assignment (pre-existing verifier ICE). Spun off: `p2/narrow-param-call-arg-skips-truncation-verifier-failure`, `p3/switch-never-matches-negative-case-label-on-int`, `p3/simd-scalar-return-does-not-splat`, `p3/enum-narrow-base-widens-with-sign-extension`. |
| `p3/uninitialized-new-array-reads` | PARTIAL - one leg queued; opt-out name RULED 2026-09-03 `init_capacity` (renamed in core/doc/test) | only the Debug poison fill for `init_capacity(n)` (option 1, allocator-level, no codegen) is queued. The 800-site `T* p = new T[n]` migration is deferred with [[raw-array-count-desugar-direction]]; the `alloc_zeroed` optimization waits for allocator measurement. The `init_uninit` NAME is unruled. |

## Constraints

- Narrow-int: preserve both folded and runtime shapes; the folded `-(i8)-128` already agrees with
  clang, only the runtime `i8 v = -128; -v` cell is open on the signed side.
- HeapAudit: `LogError` only, no new test file - extend the existing `--run` rejection coverage.
- Poison fill: Debug only, library-level behind `__active_allocator`; Release untouched.

## Adjacent

- Round-1 q19 (`--run` smoke report): the HeapAudit item is its last survivor.
- `p3/uninitialized-new-array-reads` "Landed 2026-09-02" section - what is already done.
| `p3/mixed-owning-borrow-struct-ternary-join-leaks-the-owning-arm` | LANDED 8201425 | a '?:' whose arms disagree on ownership of an owning-value struct is a compile error at the join (RejectMixedOwnershipTernaryJoin); indirect-call (Lambda/function) owning returns now ledgered so both-own joins adopt instead of leaking. Spun off: `p3/mixed-owning-borrow-pointer-ternary-join-leaks-new-arm`. |
