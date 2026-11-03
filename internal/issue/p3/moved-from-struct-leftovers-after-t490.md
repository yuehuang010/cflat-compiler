# Moved-from struct leftovers after T490

T490 (2026-10-24) made a moved-from plain CFlat struct (user dtor, no owning fields) dead at scope exit. Still open:

- (T418, master too) A by-value C++ class parameter consumed with `_ = move t` runs its dtor 3 times for 2 objects
  (`cfm(rettw())` -> stats 10130, clang 10120). The T490 r1 attempt (skip the release for parameters) fixed the count but
  deferred the release to function exit (stats inside the callee 10001 vs master 10010), unlike `_ = move` on a C++ local;
  backed out. Fix: keep the immediate release and disarm the parameter's later cleanup.
  T499 (2026-10-25) tried "callee destroys the parameter when its body always consumes it, caller skips": correct for
  direct calls, but two rounds failed on indirect calls - a function value keeps the first target's contract
  (`fp = A; if (c) fp = B;` leaks, a typed function<int(Val)> parameter destroys twice: 10240 vs clang 10120), and lambdas /
  generic function values skip the caller destroy without a callee destroy (leak). Backed out; master behaviour kept.
  RULED 2026-10-29 (maintainer): CALLEE DESTROYS, ALWAYS - every CFlat-ABI (non-extern) function destroys its own by-value
  ctor/dtor-bearing parameters on every target (MSVC convention, uniform); `_ = move t` = immediate release + local drop flag;
  one contract for direct and indirect calls, function values, lambdas and generics. Extern definitions keep the platform ABI.
  Supersedes the T499 "callee destroys only when the body always consumes" attempt. Conditional consumes (`if`, early return, loop, recursive: 20030 vs clang 20020) need the
  same contract plus the callee's conditional drop flag. Repros: scratch/repro_keep/t499/ (main checkout).
- Local-field moves are fixed by the MVFIELD runtime field-drop tracking. The equivalent heap-field path
  (`take(move p->inn)`) remains open: it still drops one extra moved-from value after three ownership-root
  tracking attempts. RULED 2026-10-29 (maintainer): ALL-ZERO IS THE NULL STATE for a by-value user-dtor field - the move
  zero-fills the source field and a user destructor is skipped on an all-zero object (the by-value analogue of the pointer
  nullptr check). One rule everywhere; MVFIELD's local drop flags may be replaced by it. Accepted: a live all-zero object
  also skips its dtor body (never unsafe, no owning fields). Supersedes the 2026-09-02 no-zero-fill drained-state line. Repro: `scratch/repro_keep/mvfield/field_matrix_noaudit.cb` (main checkout).
- Scope: structs with a raw-pointer or `int[]` view field are excluded from T490 (cautious; C-style `~Buf(){ free(data); }`)
  and still get 2 dtor runs after a move. RULED 2026-10-29 (main session, from precedent; maintainer may veto): T490 covers every plain struct - a moved-from shell is dead whatever its fields hold (a second
  `~Buf() { free(data); }` run on a bitwise-moved shell is a double free). Remove the raw-pointer / view-field exclusion.
Repros: scratch/repro_keep/t490/ (main checkout).
Found: T490 r1 review (2026-10-24).

## QUESTION FOR MAINTAINER (2026-10-30, box 7) - premise of the all-zero ruling

Branch fix/movedtor (refs/review/movedtor_r7, worktree cflat-fix-movedtor; all gates green, not landed) implements the 2026-10-29 ruling
literally. The accepted clause "a live all-zero object also skips its dtor body (never unsafe, no owning fields)" has a cost the
premise did not cover: a user dtor with SIDE EFFECTS on a type with no fields (or only zero fields) no longer runs, while its ctor does.
- Repro scratch/repro_keep/movedtor_guard/guard.cb (main checkout): `struct Guard { Guard(){ locked++; } ~Guard(){ locked--; } }` used
  as a scope guard -> master exit 10 (released), branch exit 11 (never released). Any field-less RAII guard (unlock, close, counter,
  log scope) silently stops working. Frees are never lost (owners are never all-zero).
- A live padded object whose fields are all zero: whether its dtor runs depends on the (frozen) padding bytes - nondeterministic for live
  objects; moved-from shells are memset whole, so they always skip.
- 20+ test fixtures needed an `int alive = 1;` sentinel to keep proving frees/order (the cost lands on users the same way).
Options:
  (a) keep the ruling as is (accept the RAII cost);
  (b) skip the dtor body only for MOVED-FROM objects: the move zero-fills, and the skip test is "all-zero AND the object was a move
      source" - needs a per-object drained bit only where a move can reach (fields / heap / elements), which is what MVFIELD's drop flags
      did locally; live all-zero objects always run their dtor;
  (c) skip on all-zero only for types whose dtor body reads no global state / calls nothing (pure field cleanup) - analysis-heavy.
Recommendation: (b) - the null-state analogue of `nullptr` is about drained owners, not about live values that happen to be zero.
T418 (callee destroys by-value params) is queued behind this branch.
