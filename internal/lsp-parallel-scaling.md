# LSP parallel parse scaling cap

Investigated 2026-06-07 whether removing ANTLR would help parallel multi-file parsing perf. Measured on the 20-thread dev box (Strix Point, see [[project_host_cpu_cache_topology]]) via `test_lsp.bat Release` (102 analyses), pool size via env `CFLAT_LSP_POOL_SIZE`:

| Config | Serial (pool=1) | Parallel (pool=20) | Speedup |
|---|---|---|---|
| Baseline | 11.8s | 7.4s | 1.6x |
| With `ANTLR4_USE_THREAD_LOCAL_CACHE` | 12.0s | 7.4s | 1.6x |

CPU during parallel run: ~13/20 cores busy avg, 100% peaks.

**Finding 1: `ANTLR4_USE_THREAD_LOCAL_CACHE` is a no-op for this workload.** The generated `CFlatParser.cpp`/`CFlatLexer.cpp` store prediction state (`decisionToDFA` + `sharedContextCache`) in a process-global `cflatParserStaticData` unless that macro makes it `thread_local`. The macro is NOT defined in `cflat.vcxproj` by default. Defining it (tried in all 4 configs, only gates code we compile - not in runtime headers, no ABI risk) changed nothing serial or parallel. So ANTLR's shared DFA-lock contention (documented at `ParserATNSimulator.h:174-185`) is NOT the scaling bottleneck. Reverted the macro (costs ~20x DFA-cache memory for zero benefit).

**Finding 2: real cap is redundant work, not contention.** Serial ~12 CPU-s; parallel ~7.4s x 13 cores ~= 96 CPU-s -> parallel burns ~8x more total CPU for 1.6x wall speedup. Cause: each of the N backend slots (one `LLVMBackend` instance per slot in `LspServer.cpp` pool) owns its OWN per-instance `parseTreeCache_`, so each independently re-parses the whole core-library closure (runtime.cb + transitive) on its first analysis. Serial parses stdlib once and reuses the warm cache across all 102 files; parallel re-parses it up to N times.

**Why:** removing ANTLR for a custom parser would only make each redundant re-parse cheaper, not remove the redundancy. The high-leverage fix is to stop re-parsing the stdlib per slot: share core parse trees / symbol index across slots (parse once at pool init, workers read a shared immutable copy), or load core from `--init` bitcode + a shared symbol snapshot.

**LSP does NOT use the --init bitcode cache (confirmed 2026-06-07):** `LoadCoreBitcodeIfFresh` is called only from the regular compile path (`CompileFile`, LLVMBackend.cpp:322), never from `Analyze` (the LSP path, ~line 1374). `Analyze` auto-imports runtime.cb by parsing from source (`CompileImportedFile`) and all other core imports go through the same source-parse path. This is fundamental, not an oversight: --init stores opaque LLVM bitcode (no parse trees / source spans / symbol metadata), but LSP hover/completion/go-to-def need parsed trees + symbol index, which bitcode can't supply. So "make LSP use the bitcode cache" is a DEAD END. The LSP-appropriate fix is to parse the core closure once and share the parse trees + symbol index (immutable, read-only) across backend slots.

**How to apply:** before any parser-replacement work for parallel perf, fix the per-backend core re-parse. ANTLR coupling itself is deep but tractable: the parse tree IS the AST (no separate layer); ~292 hand-written `Parse*` methods in MainListener.h walk `CFlatParser::*Context` directly (186 refs, 65 distinct context types); MainListener overrides only `enterExternalDeclaration`, rest is manual recursive descent. Pool-size knob for measuring: `CFLAT_LSP_POOL_SIZE=1`. Related: [[project_compile_perf_findings]], [[project_check_mode_parse_cache]].
