# `--check` cannot repopulate the C++ type-request cache, and a rebuild orphans it

Found 2026-09-17 during the cache-determinism review (macOS arm64, Release, master 5244c197).

## Summary

The persistent C++ type-request cache is keyed on the compiler stamp, so every rebuild orphans the whole cache. `--check` runs in batch mode, and batch mode refuses to persist type-request entries ("STORE REFUSED": 61 batch mode, 24 no cache clause in one verbose fixture run), so a `--check` after a rebuild pays the full cold cost (Test/test_cpp_interop.cb: 195-214 s) on EVERY run until some native compile of the same program repopulates the cache. Two consequences: warm-time measurements taken with `--check` right after a build are silently cold (this misled two review rounds this week, reporting 193-202 s "warm" against 31 s), and the LSP / editor path that only checks never gets warm after an upgrade.

Even when warm, `--check` still runs clang inside replay: CxxRequestStage1 15.3 s over 35 events plus EnsureCxxRequestPch 7.3 s over 20 events in a 29 s warm fixture check - the largest remaining warm cost, identical on master and on the determinism branch.

## Repro

```
./cmake_build.sh release && x64/Release/cflat --init-local
time x64/Release/cflat Test/test_cpp_interop.cb -i Test/library --check   # ~200 s
time x64/Release/cflat Test/test_cpp_interop.cb -i Test/library --check   # still ~200 s
x64/Release/cflat Test/test_cpp_interop.cb -i Test/library -o scratch/tci.out   # ~200 s, populates
time x64/Release/cflat Test/test_cpp_interop.cb -i Test/library --check   # ~30 s
```

## Fix direction

Let batch mode persist type-request entries (the refusal was a staleness guard; the determinism fix makes entries key-pure so the guard can be revisited), and key the cache on the header/request payload hash rather than the compiler stamp where the payload version already guards format changes. Separately, batch the stage-1 requests per import group so a warm check does not run clang 35 times.

Suggested bucket: p3 (performance and measurement hazard, no wrong output). Note for anyone timing warm compiles: populate with one native compile first.
