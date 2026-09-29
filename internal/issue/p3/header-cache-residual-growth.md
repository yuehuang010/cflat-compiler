# Header cache: residual growth paths after G5 (81558698)

Summary: G5 prunes dead `cheaders/v<M>` version directories. The A8 round-2 review (2026-09-28) listed the remaining in-version growth, all small:
- `sigbase.*.json` / `.ptr` files are never pruned. This is inert today, since no current cache holds one.
- A template-argument combination or `-I`/`-D` configuration that is no longer compiled keeps one generation of dead request entries, matching master's abandoned-config leak.
- `.cxxdemand.bc` grows by one file per included-header edit and is aged out after 10 minutes.
Fix direction: extend the in-version prune (the G1 path) with an age rule for these files.
