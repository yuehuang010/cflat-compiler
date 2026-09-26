# C++ request cache entry carries signatures from other headers in its incremental TU (was: owner header nondeterministic)

Bucket: p2 (wrong-context cache replay -> spurious refusals; plus warm-cache misses and growth). Found 2026-09-25 while timing
test.sh: a second, warm `./test.sh Release` added 89 new request entries (~33 MB) under
`x64/Release/.cflat/cheaders/v100/`. Runs 3 and 4 added nothing (1994 json / 1078 bc / 655 MB each time),
so it is a one-time growth: once every owner variant is cached, warm runs hit. Cost is the
duplicated entries plus clang work on the first warm run after a cache version bump.

## Repro

Run `./test.sh Release` twice with no rebuild. Entries added by run 2 repeat run-1 requests
with a different `H` (owner header) field in `cxxRequestKey`, e.g.
`|RQstd::shared_ptr<__cflat_user::GrsSigBP>|...|H.../cpp_interop_sfinae.h|...` in run 1 and
`|H.../cpp_interop_basic.h|` in run 2 (same request, same S suffixes -262/-922/-3280/-3940).

## Suspected cause

The owner header of a request group is chosen from state that depends on parallel test timing -
likely `cxx-owner-groups.json`, which every process reads and rewrites. Not verified.

## Fix direction

Make the owner choice a pure function of the compile's own imports (e.g. sorted owner set), or
drop H from the key when the request's content does not depend on it. Acceptance: a second warm
test.sh run adds 0 entries to cheaders/v<version>/.

## 2026-09-26: attempted fix reverted - the owner order masks cache contamination

0634b54f removed the shared `cxx-owner-groups.json` memo and sorted candidates canonically; it
broke the LSP bulk sweep (test_cpp_interop_template.cb L1066, `cppt.LongList` variadic ctor:
"use of undeclared identifier 'cppt'") and was reverted. Keeping import order instead fixed LSP
but failed err_cpp_diag_display_names / err_cpp_nested_missing / err_cpp_proxy_assign_refused /
err_cpp_view_decay_noncontiguous ("'std::function<int (int)>' does not name a C++ class type").
Those 4 pass on an EMPTY cache, so the order is not the root cause.

Root cause found: request cache entries are keyed on the primary group's headers, but the
incremental executor reuses one clang TU per primary group (GetCxxIncrementalGroup; dependency
headers do not split it), so a request answered inside a compile that also imported, e.g.,
cpp_interop_basic.h stores signatures from that header under a key that names only `vector`
(+ cpp_interop_nest.h). Dozens of v100 entries (`std::vector<nest::Cell>`, `std::__wrap_iter<...>`)
carry `cppi::HoldsStdFunction` / `OpsSink::apply_fn`. A narrower compile that hits one replays
signatures it cannot resolve. Which entries are polluted depends on write order and owner pick -
so every ordering breaks something different, and the old memo only hid it by keeping picks stable.

Fix direction: the cached result must contain only what the key describes - either key the entry
on every header in the TU that produced it, or filter the harvested sigs/records to declarations
from the key's headers (and their includes) before storing. Then the owner memo can go.
Round 1/2 attempt notes: scratch/ownerfix_round{1,2}_REPORT.md (local only).
