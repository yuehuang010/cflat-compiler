# `take(c ? default : x)` into C++: remaining wrong shapes (T391 + T418 + T420)

T418 and T420 landed:
- `c ? default : local` (any `?:` tree whose leaves are `default` or a live local of a nontrivial C++
  class) into a C++ free function reached through a generated call wrapper, or into a C++ member
  function, is no longer joined in CFlat: the selector and the locals are passed to a unique wrapper
  that spells the C++ conditional itself (MainListener_Expressions.cpp BuildCxxDeferredDefaultTernary,
  LLVMBackend_CInterop.cpp appendDirectConstruction in RequestCxxFreeFunction). clang-exact for the
  unbound and bound wrapper by-value cells, `take2` with two ternaries, nested `?:`, a conversion-
  operator arm into a bound wrapper, `rr(T&&)` + `rr(const T&)`, and member const& at c=0. A directly
  bound free function takes this route only when the local's class differs from the parameter's
  (conversion arm: `tref(c ? default : lk)` and the by-value form, which was refused).
- An unbound C++ free wrapper call counts as a C++ call argument when an argument has a `default`
  arm, so the T391 statement-end destroy covers the shapes the deferral does not take (field arm).
- T420 extends wrapper deferral to a direct field read from an inert live local, a `move` local leaf
  (the wrapper uses `std::move` only when selected). A raw-signature conversion-target branch was
  dropped in review: no probe showed it changing a result versus master.

Repros: scratch/t418/p in the T418 worktree (h.h include form = the wrapper path: the included header
is outside the imported header's directory; every Test/library header is in scope for the test files,
so the wrapper path has no in-repo leg). Values: value*1e7 + ctor/copy/move/ca/ma/dtor/live digits;
now (clang), c=0 / c=1.

Still wrong:
- When the first call has no usable callee signature, keep the conditional's place type; do not
  guess its target from a unique source conversion. `scratch/t420/look.cb` (`cpptw.take`, Look ->
  Twin) remains at master values 41000010 / 41000010, while clang gives 41000010 / 1000010. The
  parameter cannot be known before that call, so this Tier 3 shape is deferred.
- Call arm through the wrapper path (`take(c ? default : rettw())`, also as one of two arguments):
  refused ("'?:' arms handle ownership differently"); clang 1000010 / 1000010. The direct path accepts.
  T421 (parked 2026-10-23): a zero-arg call-arm deferral made the one-argument form clang-exact, but the
  two-argument form (trailing `default` with no target) failed after 3 attempts, and the wrapper path has
  no in-repo leg (needs the external shim layout). Patches + report: scratch/repro_keep/box1022/t421/.
- `take(c ? default : (default))` through the wrapper path is out of scope: both arms are untyped
  `default`, so the only possible target is the callee parameter, unknown before the first wrapper call
  (same boundary as the T420 conversion arm: never guess the target).
- C++ constructor const& `rv.Holder(c ? default : tw)`: c=0 401000010 (clang 401100020, the prvalue
  copy). Constructor calls do not take the deferred route.
- Wrapper cost (not ternary-specific): a by-value C++ class argument moves once more through the CFlat
  wrapper (`take(rettw())` and `take(default)` 1010020 vs clang 1000010).
- Deferral is skipped when a LATER argument may write a leaf (anything but literals, `default`, plain
  reads of non-class locals, or another deferrable `?:` over such reads), so those calls keep the
  CFlat join. The free-wrapper `take(c ? default : x, mutate(x))` reads 902 (clang 702) on master and
  with T418 - the join copy is made after the later argument runs.
