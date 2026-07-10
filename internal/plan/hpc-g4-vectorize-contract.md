# G4: FP-Math Tiers via vectorize(contract) / vectorize(reassoc) - Detailed Plan

Status: LANDED 2026-07-09 (Windows; branch feature/hpc-g4-vectorize-fp)
Created: 2026-07-09
Parent: internal/plan/hpc-gaps.md (G4)

Goal: give kernels FMA contraction (a*b+c -> one vfmadd on FMA targets)
with a scoped, source-visible spelling. Re-scoped from the parent plan's
`fastmath { }` block by user decision: NO new keyword - extend the
existing `vectorize` soft keyword to `vectorize(contract)`. The mental
model stays coherent: `vectorize` is already the one place a loop opts
into FP-order relaxation (reduction reassociation), so contraction lives
in the same spelling.

Scope update 2026-07-09 (user decision): `vectorize(reassoc)` is IN
scope as the second tier - see Semantics. Out of scope (deferred until
asked for): `vectorize(fast)` and the fine-grained flags (nsz/arcp/afn,
and especially nnan/ninf, which turn data conditions into poison), a
whole-program `--fp-contract` CLI flag, any contraction outside
vectorize loops. Serial-FMA users (Horner) already
have the explicit escape hatch: scalar `fma()` in core/math.cb:197 and
simd `fma` both exist and lower to llvm.fma.

---

## Where everything hooks in (from codebase survey 2026-07-09)

- Grammar: `vectorizeStatement : 'vectorize' iterationStatement ;`
  (CFlat.g4:544-546), soft keyword like `lock`. Parenthesized-argument
  precedent to copy: `lockClause : 'lock' '(' lockArgList ')'`
  (CFlat.g4:548-558).
- Detection/rejection: ParseStatement, MainListener.h:3574-3622
  (vectorizeActive_/vectorizeLine_ latched; do-while/foreach rejected).
  State flags at MainListener.h:1829-1838.
- Loop-body emission brackets (the FMF save/set/restore sites): the
  currentVectorizeBodyLine_ save/restore around ParseControlledBody -
  while-form MainListener.h:4111-4117, for-form :4172-4178.
- NO existing FMF machinery anywhere: no setFastMathFlags, no
  Create*FMF; scalar FP ops are emitted flagless (LLVMBackend.h:10393,
  10397, 10589, 10599). Only prior art is setHasAllowReassoc on the simd
  reduce_add call (MainListener.h:11220). Builder-default FMF is a
  greenfield hook - nothing to conflict with.
- Checked-contract enforcement (LLVMBackend.cpp:1974-2061) and the
  llvm.loop metadata (AttachVectorizeHintToCurrentLatch,
  LLVMBackend.h:14211-14237) are orthogonal: contraction does not change
  vectorization legality. NO change there.
- ForwardRefScanner: no override exists for vectorizeStatement and none
  is needed (statement-level only, never file scope). Both
  ParseDeclarationSpecifiers copies untouched - G4 constraint from the
  parent plan holds by construction.
- Reduction reassoc today comes from the llvm.loop.vectorize.enable hint
  (LoopVectorize's allowReordering honors the explicit force) - that is
  why plain `vectorize` FP reductions already reorder without any FMF.
  `contract` is additive and independent of that mechanism.

---

## Design

### Syntax

```
vectorizeStatement
    : 'vectorize' ('(' Identifier ')')? iterationStatement
    ;
```

- Argument is a bare identifier, validated in ParseStatement by string
  match (soft-keyword style, like `move`): accepted values are
  `contract` and `reassoc`. These are TIERS, not composable flags -
  `reassoc` implies contract (if you allow reordering you have accepted
  fusion), so there is no argument-list plumbing and dispatch stays a
  two-way string match. Anything else ->
  LogErrorContext("vectorize flag must be 'contract' or 'reassoc'") and
  treat as plain vectorize (log-and-continue, but RETURN-style early-out
  per the G3 hardening convention - do not index into a null ctx).
- Plain `vectorize` is byte-for-byte unchanged (backward compatible).
- Grammar edit triggers ANTLR regeneration on rebuild; no lexer token
  added (stays a soft keyword).

### Semantics

Two tiers, both = plain vectorize (same checked contract, same
metadata, same enforcement) PLUS fast-math flags on every FP
instruction emitted for the loop's lexical BODY:

- `vectorize(contract)`: sets `contract` only. Allows the backend to
  fuse mul+add chains into fma on targets that have it. Contraction
  only removes one intermediate rounding; it never reorders across
  statements - the "safe" tier.
- `vectorize(reassoc)`: sets `reassoc` AND `contract`. Extends the
  reordering permission plain vectorize already grants to the REDUCTION
  chain (via the loop hint) to ALL FP math in the body - the optimizer
  may regroup expressions, split accumulators, and vectorize FP
  patterns the hint alone cannot. Results can differ from serial by
  more than the reduction ULP drift; document that this is the tier
  for "throughput over reproducibility" kernels.

VERIFY-AT-IMPLEMENTATION (reassoc tier): check whether the transforms
we advertise (accumulator splitting, expression regrouping) additionally
require `nsz` in practice on LLVM 18 - some reassociation patterns
guard on it. If so, have the reassoc tier set reassoc+contract+nsz and
record that in the as-landed notes (nsz is benign: +0.0/-0.0
interchangeable); do NOT silently add any other flag.

Region = the lexical body (everything under ParseControlledBody),
including nested non-vectorize loops and calls that get inlined. The
loop condition/increment emitted outside the brackets stay flagless -
irrelevant for fusion (they are integer/compare ops in a counted loop).

### Implementation (MainListener.h + nothing else in codegen)

1. Parse: in the vectorizeStatement branch (:3581), read the optional
   identifier; set a new `vectorizeFpTier_` (enum: none/contract/reassoc)
   alongside vectorizeActive_/vectorizeLine_ (:1829-1838), latched and
   cleared in the iterationStatement branch (:4029-4035) exactly like
   the others (so nested loops do not inherit it).
2. Emit: at the two body brackets (:4111-4117 while, :4172-4178 for),
   wrap ParseControlledBody:
   ```
   llvm::FastMathFlags prevFMF = builder->getFastMathFlags();
   if (tier != none) { llvm::FastMathFlags f = prevFMF;
                       f.setAllowContract(true);
                       if (tier == reassoc) { f.setAllowReassoc(true); }
                       builder->setFastMathFlags(f); }
   ParseControlledBody(...);
   builder->setFastMathFlags(prevFMF);
   ```
   Builder-default FMF applies only to FP ops - integer/pointer emission
   is unaffected. Save/restore (not clear) so a future outer FMF scope
   composes.
3. ResetForReanalysis: clear vectorizeFpTier_ with the other transient
   flags (the parseTreeCache_ rule - a flag left set by an aborted
   compile must not leak into the next file).

VERIFY-AT-IMPLEMENTATION item: confirm lambda/closure bodies written
inside a vectorize body are NOT emitted through the same builder while
the flag is set (deferred emission would leak contraction out of the
region). If they are emitted inline through this builder, either accept
it (document: lambdas defined in the region inherit contract - defensible
lexical-scope semantics) or save/restore around the lambda-emission path.
Decide by reading the emission path, and record the outcome in the
as-landed notes; do not guess.

### Interaction notes (document in HPC.md)

- With the checked contract: unchanged. contract neither helps nor hurts
  vectorizability; the -O2-only enforcement gate is untouched. The FMF
  is emitted at ALL opt levels (harmless at -O0; ISel may still fuse).
- With reductions: a `vectorize(contract)` FP reduction gets BOTH the
  existing hint-driven reassociation AND fused multiply-adds in the
  vector body - this is the dot-product payoff (HPC.md:205's "write
  acc + a*b" note gets updated to point here).
- With simd.fma: simd/scalar fma() remain the EXACT single-rounding
  operation; contract is permission to fuse, not a guarantee. Keep the
  existing HPC.md fma caveat and cross-link.

---

## Verification

1. .ll inspection (--out-lli): body fadd/fmul carry `contract` (and
   `reassoc` for the reassoc tier); ops outside the loop and in sibling
   loops do not; nested vectorize inside plain (and vice versa) scope
   correctly. With --cpu native -O2 the kernel shows llvm.fmuladd/fma
   in the optimized IR (or vfmadd in asm).
2. Functional: extend Test/test_vectorize.cb (hand-rolled fails counter
   style, no assert macro): a mul-add kernel under vectorize(contract)
   compared against serial with a small tolerance (contraction legally
   changes the last ulp - do NOT require exact equality; float-literal
   trap also applies), plus an exact-equality case built from values
   where fused == unfused (small integers in double). For reassoc: a
   sum-of-products kernel vs serial with a looser relative tolerance
   (reassociation drift scales with n), again with an exact case built
   from small integers where every grouping is exact.
3. Error test: Test/errors/err_vectorize_bad_flag.cb -
   expect_error("vectorize flag") on `vectorize(fast)` - expect_error
   form needs no script change (Test/vectorize/ + test_hpc.bat additions
   only if an expect_fail-style negative is genuinely needed; prefer
   Test/errors).
4. The three existing Test/vectorize/neg_*.cb and test_vectorize.cb
   must stay green untouched (plain-vectorize regression).
5. Perf evidence: performance/hpc kernel (dot-product or axpy-with-mul
   variant), --cpu native -O2, contract vs plain - expect measurable
   throughput gain from fmadd fusion; record numbers in as-landed notes.
   Fresh processes, pinned, per the hybrid-box measurement rules
   (cpu_mask_perf_cores() now exists for exactly this).
6. Gates: build Release; test.bat green; test_lsp.bat green (grammar +
   MainListener touched); example.bat green; doc/HPC.md updated in the
   same change (vectorize section :35-86 gains a contract subsection;
   reduction paragraph :79-84 amended; :205 FMA note updated).

## Sequencing / delegation

Single phase - the change is small but sits in grammar + ParseStatement
+ builder state: opus tier per the delegation table (grammar + codegen).
Worktree + agent per the established flow. Disjoint from all G7 items -
the BLAS example can run in parallel at sonnet tier if started.

---

## Implementation notes (as landed) - 2026-07-09

Landed exactly as designed; both tiers work. Files changed:
- `cflat/CFlat.g4`: vectorizeStatement gains `('(' Identifier ')')?`.
- `cflat/MainListener.h`: `enum class VectorizeFpTier {None,Contract,Reassoc}`
  + `vectorizeFpTier_` flag + static `ApplyVectorizeFpTier(compiler, tier)`
  helper; parse-time validation in the vectorizeStatement branch (accepts
  `contract`/`reassoc`, else LogErrorContext + return); tier captured/cleared
  at the iteration-statement consume site next to `vectorizeActive_`; FMF
  save/set/restore wrapped around `ParseControlledBody` at both the while-form
  and for-form body brackets.
- `Test/test_vectorize.cb`: four new functions (contract approx+exact,
  reassoc approx+exact), wired into main's fails counter.
- `Test/errors/err_vectorize_bad_flag.cb`: new error test on `vectorize(fast)`.
- `doc/HPC.md`: FP-math-tiers subsection, reduction bullet amended, simd `fma`
  caveat cross-linked to `vectorize(contract)`.

Deviations from the plan:
- ResetForReanalysis: the plan said to clear `vectorizeFpTier_` there, but
  `ResetForReanalysis` is an `LLVMBackend` method and the vectorize flags are
  `MainListener` members. `MainListener` is constructed fresh per analysis
  (`std::make_unique<MainListener>` in LLVMBackend.cpp / LspServer.cpp), so its
  members default-init each run - exactly how `vectorizeActive_` is already
  handled (it is NOT in ResetForReanalysis either). Leak prevention is fully
  covered by the default member initializer (`None`) plus the consume-site
  clear next to `vectorizeActive_ = false`. No ResetForReanalysis edit needed;
  `vectorizeFpTier_` mirrors `vectorizeActive_` one-for-one.

VERIFY-AT-IMPLEMENTATION outcomes:
- (a) Lambda/closure bodies inside a vectorize body: ACCEPTED (inherit the
  tier). Read the emission path: `ParseLambdaExpression` emits the lambda body
  inline through the same `compiler->builder` (`ParseBlockItemList` /
  `ParseAssignmentExpression`), guarded by `SaveBuilderState`/`RestoreBuilderState`.
  `BuilderState` snapshots only the insert point + function/return + pending
  temps; it does NOT snapshot fast-math flags (and `saveIP`/`restoreIP` save
  only the insertion point). So a lambda written lexically inside the loop body
  has its FP ops emitted while the FMF is set and inherits contract/reassoc.
  This does NOT leak past the region: the FMF is restored right after
  `ParseControlledBody`, so nothing after the loop (or in sibling/outer loops)
  is affected - verified in the .ll (see below). Defensible lexical-scope
  semantics; documented in HPC.md. No extra save/restore added.
- (b) reassoc + nsz: NOT needed. Empirically, `vectorize(reassoc)` on a
  sum-of-products at `-O2 --cpu x86-64-v3` already splits the reduction into
  four independent `<4 x double>` accumulators (accumulator splitting) and
  finishes with `llvm.vector.reduce.fadd`, all carrying `reassoc contract` and
  NO nsz. LLVM 18 initializes the split-lane identities with `-0.0` itself, so
  it does not require the source FMF to carry nsz. The reassoc tier therefore
  sets `reassoc + contract` only (no nsz), as designed.

.ll evidence (unoptimized, --out-lli):
- `vectorize(contract)` body: `fmul contract` / `fadd contract`.
- `vectorize(reassoc)` body: `fmul reassoc contract` / `fadd reassoc contract`.
- Plain `vectorize` body, ops outside any loop, and sibling plain loops: bare
  `fmul` / `fadd` (no flags).
- Nested plain loop inside a tiered loop: inner body inherits `contract`.
- Tiered loop inside a plain loop: only the inner tiered body carries `contract`;
  the outer plain loop's ops before AND after the nested loop are flagless
  (balanced save/restore).

Optimized-IR / codegen evidence (-O2 --cpu x86-64-v3):
- contract mul-add kernel vectorizes to `<4 x double>` with `fmul/fadd contract`,
  and `llc -mcpu=x86-64-v3` lowers it to `vfmadd132pd` / `vfmadd132sd`.
- The same kernel under PLAIN `vectorize` produces 0 `vfmadd` and 0 `contract`
  flags - confirming the tier is what enables the fusion.
- Note: `--cpu native` resolves to baseline SSE2 on this box (no FMA); use
  `x86-64-v3` for FMA evidence, per the plan.

Gates (Release, all green):
- `cmake_build.bat release`: build + ANTLR regen succeeded.
- `test.bat Release`: all tests passed (incl. test_vectorize + err test).
- `test_hpc.bat Release`: all HPC checks passed (test_vectorize `--run -O2`
  positive + three vectorize negatives + span-noalias).
- `test_lsp.bat Release`: 202 passed, 0 failed. (A first run showed 4 transient
  failures in `example/vcpkg/*` from a cold vcpkg header-bind cache under the
  parallel sweep; a warm-cache re-run is 202/0 and the baseline exe passes them
  too - not a regression, and orthogonal to this change.)
- `example.bat Release`: 88 passed, 0 failed, 24 skipped.

Skipped per instructions: the perf-evidence step (main session benchmarks
after landing). Both-pass ParseDeclarationSpecifiers untouched (verified: the
diff does not reference it). Vectorize enforcement (LLVMBackend.cpp:1974-2061)
and loop metadata (AttachVectorizeHintToCurrentLatch) untouched.
