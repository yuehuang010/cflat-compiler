# test_libs torch: intermittent "'IntArrayRef' is not a member of namespace 'c10'" on the first parallel run after an exe rebuild

**Seen:** 2026-09-28 13:47, `buildci.sh --nightly` on master 1898482a, LIBS stage (test_libs.sh tiers 1-3,
JOBS=4, shared `out/libs-cache`) right after a full Release rebuild (32 objects relinked, new
CompilerBuildStamp) and a test.sh run (load 13). Two cases failed at 5-6 s, both on the same line shape:

```
torch_03_modules.cb(104,28): 'IntArrayRef' is not a member of namespace 'c10' (C++ free function
'c10.IntArrayRef' could not be bound (clang: no matching constructor for initialization of
'::c10::IntArrayRef' (aka 'ArrayRef<long long>')))
torch_04_training.cb(121,28): same text
```

Source: `c10.IntArrayRef sizes = c10.IntArrayRef(&shape[0], 3);` with `long[3] shape`. The alias
`c10::IntArrayRef` (typedef `ArrayRef<int64_t>`) was NOT known as a type in those two processes, so the
call fell through to the free-function binder and clang rejected the wrapper. The other 9 torch cases
in the same run passed; the two failing ones are the only cases constructing an alias-named class
this way.

**Not reproduced (9 attempts, same commit):** `touch x64/Release/cflat` + tier 3 at -j 4 (x7, incl. a
5-run loop), touch + -j 1, `touch x64/Release/core/*.cb` + -j 4, and a faithful rerun (touch
cflat/main.cpp, relink, full `buildci.sh --nightly`) - all 31/31. A nightly run at 12:41 on the same
tree WITHOUT a rebuild also passed. Log: scratch/buildci_final_2026-09-28.log (the per-case
compile.log was overwritten by the reruns).

**Hypothesis:** cross-process race in the C header disk cache after a stamp change. The header entry key
and the request PCH key fold CompilerBuildStamp() (LLVMBackend_CInterop.cpp ~5942 / ~18249) while request
chunk entries do not, so the first run after a rebuild has four processes re-parsing chunk 0 of the
same torch group concurrently and rewriting the entry. Each FILE is written temp + rename (atomic), but an
entry is several files (header entry, demand chunks, owner-groups json), and the in-process "read waits
for the pending write of the same entry" guard (02ae6de0) does not cover another process: a reader can
pair a fresh header entry with another process's half-written chunk set (or the reverse) and replay a
chunk that predates the alias registration. Timing-dependent: needs the four cold siblings to overlap
their write windows, which the CI run's leftover load made likelier.

**Fix direction:** (1) make the multi-file entry atomic across processes - write the whole entry under a
per-process temp directory and rename the directory, or a lock file (O_EXCL) around entry
create/replace with readers retrying; (2) until then, a reader that finds an entry whose files carry
mixed stamps must treat it as a miss and re-parse; (3) a regression probe: run torch_01-04 concurrently
in a loop after `touch`ing the exe, under `nice`-induced load, until the alias miss reproduces, then add
the stamp-consistency check and re-run the loop.

**Acceptance:** 20 consecutive `touch x64/Release/cflat; ./test_libs.sh -t 3` runs green under load
(e.g. a concurrent `bash test.sh Release -j 4`), and the per-file stamp check has an err_ test or a
-v line proving the miss path fires on a deliberately mixed entry.
