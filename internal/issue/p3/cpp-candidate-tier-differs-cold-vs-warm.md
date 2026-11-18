# C++ owner candidate tiers differ cold vs warm, so a warm compile re-keys some requests

## Summary

`CandidateCxxGroupsFor` (cflat/LLVMBackend_CInterop.cpp) ranks import groups by
`publishedNames`, header-is-name, then `namespaces`, then the rest. `namespaces` (and some
`publishedNames`) are seeded by requests as they run under `activeCxxRequestGroup_`, and a warm
cache hit seeds them differently from the cold request that produced it. So the tier a group lands
in - and the first candidate that declares the name - can differ between a cold and a warm compile
of the same source. Each pick is correct for its own key (the request cache drops declarations its
headers do not include), but the warm compile stores a second entry under a different `|H` set.

## Repro

Fresh local cache, `bash test.sh Release` three times: 1920 -> 1994 -> 1994 C++ header cache
entries. The 71 new entries of run 2 are the same `RQ` requests as run 1 under another primary
group, e.g. `std::shared_ptr<__cflat_user::SharedSelf>` (Test/test_cpp_interop_bridge.cb) is keyed
on `cpp_interop_sfinae.h` cold and on `cpp_interop_basic.h` warm. Run 3 adds nothing (fixed point).
Isolated, the bridge test alone is stable (28 -> 28); the drift needs the suite's shared cache.

## Root cause

Tier membership is compile-history state, not a function of the compile's imports alone. The
removed `cxx-owner-groups.json` memo used to pin the first pick and hid this.

## Fix direction

Derive tier membership from import-time facts only (header harvest, cached with the header entry),
or record the namespaces a cache hit carries so replay seeds the same state as the cold request.
Acceptance: three fresh-cache suite runs end at the same entry count after run 1.

## Repro status (2026-11-10, unchanged master build, fresh local cache, test.sh Release -j 4)

- Run 1 cold: 36996 `.rq` / 37374 `.json` entries under `x64/Release/.cflat/cheaders`, 1960 passed.
- Run 2 warm: 36998 / 37376 (+2), 1960 passed. Run 3 not taken.
- Reduced probes (single program `SharedSelf : cppt.ModuleBase` with `std.shared_ptr`, plus an
  unrelated `vector` import in the same cache) show no drift: stores 0 on every warm step.
- No reduced repro yet; the drift needs the suite's shared cache. Candidate mechanism: a request
  run under a group seeds `namespaces` (CHeaderDemand seedNamespace / CollectHeaderNamespaceNamesParallel
  on the request's includes); a request-cache hit does not replay those seeds, so
  CandidateCxxGroupsFor ranks groups differently warm.
