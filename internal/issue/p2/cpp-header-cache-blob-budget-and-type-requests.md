# C++ header disk cache: type requests are not persisted, libtorch compiles stay ~190 s warm

## Summary

`import cpp "torch/torch.h" cache;` compiles a small libtorch program (scratch/ladder/torch/t4.cb)
in about 214 s cold and 191 s warm (Release, arm64 macOS, 2026-09-13). The header entry itself
now hits on the warm run; the remaining time is C++ type requests.

Part 1 LANDED 2026-09-13 (cxI1, cache version 51): the companion-module bitcode is stored as a
raw sidecar `<key>.bc` next to the JSON entry (the JSON keeps `cxxbc: {file, len, hash}`); a
missing, truncated or hash-mismatched sidecar is a whole-entry miss and the entry is rewritten;
the base64 blob budget (`kMaxDiskCachedCxxBitcode`) is gone. torch.h: 33 MB JSON + 37 MB `.bc`.

Part 2 OPEN - hold LIFTED 2026-09-15 (maintainer: interop is stable enough; keep an eye on staleness): C++ type requests
(`RequestCxxForeignType`, batch/wrapper/function-template paths in
cflat/LLVMBackend_CInterop.cpp) are cached only in the process-wide in-memory `cFileSigCache_`.
Every cflat invocation re-runs all of them: each is a stage-1 parse plus a stage-2 parse+CodeGen
against the request PCH; for t4 that is most of the ~190 s.

## Why part 2 is held

Unlike the C header cache (context-free: header hash + version identify the result), a type
request result depends on process state at request time (which identities are already
registered, group using-directives, spelling reverse maps) and on the compiler build (the
generated stub text and the registration code change every round). A stale hit does not fail at
compile time; it produces wrong wrappers that fail at link or run time. While request semantics
are still changing, that cost outweighs the warm-compile win.

## Repro

scratch/ladder/torch/t4.cb with ` cache` added to the import, compiled twice with the recipe in
scratch/ladder/torch/run_all.sh; `-v` and sum the `extraction stage` lines.

## Fix direction for part 2 (ruled 2026-09-15: go)

- Persist only the clang-side result of a request (extraction JSON + companion bitcode), keyed
  by a hash of the FULL generated stub source + group header hash + include/define flags +
  compiler build id; always re-run the in-process registration from that result.
- Never persist tentative or failed requests; never write under `--run` or LSP; prune with the
  owning header entry.
- Opt-in through the import group's `cache` clause (assumed; ruling pending).
- Target: warm t4 well under 30 s. Cheaper first win for ladder turnaround: run
  scratch/ladder/torch/run_all.sh rungs 3-4 at a time.
