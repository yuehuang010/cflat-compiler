# internal/

Compiler-internal notes and hard-won gotchas. **Not** public-facing API docs - that's
`doc/`. These capture the "why" behind non-obvious implementation choices and the traps
that cost real debugging time. Read before touching the relevant subsystem.

| File | Topic |
|------|-------|
| [intrinsics.md](intrinsics.md) | Adding a compiler intrinsic: call-site dispatch, the 4-edit pattern, `__X86__` guard |
| [operator-parsing.md](operator-parsing.md) | `>>` is two tokens; Parse fns must loop over operators |
| [function-table-method-collision.md](function-table-method-collision.md) | `IsMethod` flag stops struct methods shadowing top-level functions as fn-pointers |
| [vectorize.md](vectorize.md) | `vectorize` keyword enforcement; root cause was missing TargetMachine/TTI in the pass builder |
| [noalias-arrayview.md](noalias-arrayview.md) | Thin `int[]` array-view = `int*` repr + noalias by spelling; one-way bind enforcement |
| [span-view.md](span-view.md) | `span<T>`/`view<T>`; scoped `!alias.scope`/`!noalias` metadata for by-value struct fields |
| [simd-type.md](simd-type.md) | `simd<T,N>` primitive vector type; dual-JSON ser/deser sites; vector op dispatch traps |
| [parallel-helpers.md](parallel-helpers.md) | `core/parallel.cb` authoring gotchas (`new T*[n]`, lambda `move`, generic-fn-as-fnptr) |
| [asan.md](asan.md) | `--asan` Windows/COFF fixes (`asan-force-dynamic-shadow`, NoAddress globals) |
| [lsp-parallel-scaling.md](lsp-parallel-scaling.md) | LSP parse scaling cap; ANTLR thread-local cache is a no-op; share the core parse cache |
| [sixel-aspect.md](sixel-aspect.md) | Sixel pixel aspect-ratio correction for round circles in example renderers |
| [core-deploy-staleness.md](core-deploy-staleness.md) | test.bat runs the core deployed at `x64/<cfg>/core/`, not `cflat/core/` - rebuild after core edits or tests validate stale stdlib |
| [developer-note.md](developer-note.md) | Standing design decisions: no type meta system (types are strings + flat flags); `using` alias string-only resolution - the 3 suffix-peel sites, the intentional bare-identifier namespace fallthrough; `char*`/`string` one-way coercion (no implicit `.data()` decay; non-owning wrap is move-copied into containers) |
| [string-concat-temp-flush.md](string-concat-temp-flush.md) | Owned-string temporary cleanup model: register on operator result, unregister on every owner sink (double-free trap), flush same-block only |
| [c-interop-anon-records.md](c-interop-anon-records.md) | C record mapper: `__anonN` synthesis for anonymous/unnamed members, bitfield side-table + transparent access, cheaders cache version discipline, folded-constant display, by-design unmappables (MIDL/COM) |
| [language-features.md](language-features.md) | Full CFlat feature list (generics, interfaces, arrays, module system, brace-init) + ownership/move + compile-time features; moved from CLAUDE.md |
| [stdlib-reference.md](stdlib-reference.md) | Full `core/*.cb` standard library table with exports and import requirements; moved from CLAUDE.md |
| [performance-benchmarks.md](performance-benchmarks.md) | Benchmark files, reference throughput table, stream/channel design notes; moved from CLAUDE.md |
