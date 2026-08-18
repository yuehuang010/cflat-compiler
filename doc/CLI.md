# Command-Line Reference

Every switch accepted by `cflat.exe`, grouped by purpose. This page is the human-readable
companion to `cflat --help`; both derive from the flag registrations in
[`cflat/main.cpp`](../cflat/main.cpp), so when a switch changes, update both.

```
cflat <input> [options]
cflat --init | --init-local | --init-clear | --init-clear-local | --version | --help | --print-supported-cpus | --print-host-cpu
cflat lsp                       # run the Language Server (see the VS Code extension)
cflat lsp --lsp-pool-size N      # cap concurrent analyses to N backends/workers
```

The `lsp` subcommand accepts `--verbose`/`-v`, `--import-dir`/`-i <dir>` (repeatable),
`--lsp-pool-size <N>`, and `-ftime-trace`. With the trace switch, each analysis writes a
Chrome trace under the system temporary directory's `cflat-lsp-traces` folder; only the
latest 64 traces from the server process are retained. The pool size bounds how many
analyses run in parallel (one backend and worker thread per slot); it defaults to the
logical CPU count. The switch takes precedence over the `CFLAT_LSP_POOL_SIZE` environment
variable.

`<input>` is the source file to compile. A `.cb` file is CFlat; a `.c` file is real C
(compiled by clang-cl and linked in); a `.h`/`.hpp`/`.hh` file is a C header binding. Extra
positionals are treated as additional `.c` inputs to link. See
[`doc/C_INTEROP.md`](C_INTEROP.md) for the C interop rules.

## Output

| Switch | Short | Value | Description |
|--------|-------|-------|-------------|
| `--output` | `-o` | path | Output native executable (`.exe`). Linking is what merges `.c`/`.lib` inputs, so C interop requires this. |
| `--out-lli` | `-l` | path | Write the LLVM IR (`.ll`) - the final, optimized IR that lands in the object. |
| `--bitcode` | `-b` | path | Write LLVM bitcode (`.bc`). |
| `--nologo` | | | Hide the progress and summary lines (`PASS:`, `Checked N file(s)`, `Emitted ...`). Useful for scripting. |
| `--version` | | | Print the version and exit. There is no startup banner; the version prints only here. |

You can combine `-o` with `-l`/`-b` to emit the exe and the IR/bitcode in one pass.

## Restricted policy (`--isolated`)

`--isolated <policy.json>` validates a CFlat compilation against a versioned restricted
policy. It is a compiler-side validation mode, not a new output mode. It is currently
available on macOS hosts only; on Windows and Linux hosts the flag is rejected with
`policy-output-unsupported` until enforcement is verified there. Use it with `--check`,
`--out-lli`/`-l`, or `--bitcode`/`-b`:

```bash
cflat app.cb --isolated policy.json --check
cflat app.cb --isolated policy.json --out-lli app.ll
cflat app.cb --isolated policy.json -o app --isolated-manifest app.manifest.json
```

The policy is a UTF-8 JSON object with these required fields and optional `limits`:

```json
{
  "version": 1,
  "language": "restricted-v1",
  "capabilities": {
    "stdio": "allow", "clock": "deny", "random": "deny", "filesystem": "deny",
    "network": "deny", "ui": "deny", "process": "deny", "threads": "deny"
  },
  "limits": { "heap_bytes": 1048576, "max_threads": 1 }
}
```

`capabilities` must name all eight capabilities exactly, with each value set to `allow` or
`deny`. `limits` is optional and may contain only positive integer `heap_bytes` or `heap_mb`
(at most one), and positive integer `max_threads`. `heap_mb` is mebibytes. `max_threads` must
be `1` when `threads` is `deny`. Unknown keys, duplicate keys, wrong types, zero, negative,
floating-point, and overflowing numbers are invalid.

The compiler enforces all eight denied capabilities: stdio, clock, random, filesystem, network,
ui, process, and threads. The final LLVM module is checked against a sealed positive runtime
export inventory; unknown external symbols and unknown LLVM intrinsics are rejected as
`policy-module-denied`. Heap limits are enforced by compiler-inserted atomic accounting around
the allocator, and `max_threads` limits concurrent CFlat threads including the main thread.
Exceeding either limit writes `isolated runtime error: ... exceeded` to stderr when stdio is
allowed, then aborts; with stdio denied it aborts without a message. These are compiler-side
limits only and are not an OS security boundary.

`--isolated` cannot be combined with `--run`, positional `.c` inputs,
`--asan`, or `--heap-audit`. Native interop imports and prebuilt native library options are
also outside the restricted profile.

On native macOS arm64, `-o` is supported after a post-link Mach-O audit. Other target and host
combinations still reject isolated `-o` as `policy-output-unsupported`; the audit checks load
commands, imported symbols, and writable/executable segments.

`--isolated-manifest <path>` requires `--isolated` and describes one compilation. It is rejected
for a multi-file `--check` batch. The manifest records policy and module metadata and hashes the
bound output. With multiple outputs, it binds to bitcode if present, otherwise to LLVM IR,
otherwise to the executable; `--check` has no `output.sha256`.

This is compiler-side validation only and is NOT a security boundary. It does not provide an
OS sandbox or contain the compiler or generated program; a service must provide separate
containment and runtime authorization.

## Execution (`--run`)

| Switch | Value | Description |
|--------|-------|-------------|
| `--run` | | JIT-compile and run the program in-process - no exe on disk. The process exit code is the program's exit code. |

`--run` is the fastest edit-compile-run loop: no linking, no temp `.exe`, no sibling DLL
copies. The full `-O2` pipeline (if requested) runs before execution.

**Entry point.** `--run` looks up a function named `main`, which must be one of:

```cpp
extern int main() { ... }                       // no arguments
extern int main(int argc, char** argv) { ... }  // receives program arguments
```

**Program arguments.** Everything after a bare `--` on the command line is passed to an
`int main(int argc, char** argv)` entry, matching the usual C convention:

- `argv[0]` is the source file name (e.g. `app.cb`).
- `argv[1..argc-1]` are the strings after `--`, in order.
- `argv[argc]` is `nullptr`.

```bash
cflat app.cb --run -- alpha beta     # main runs with argc==3, argv={"app.cb","alpha","beta",NULL}
```

The `--` separator ends compiler-option parsing; everything after it is taken verbatim as a
program argument, so program args are never mistaken for compiler flags or source files.

**Restrictions.**

- *Read-only.* `--run` writes nothing to disk, so it cannot be combined with `-o`/`--output`,
  `-l`/`--out-lli`, or `-b`/`--bitcode`. To get the IR *and* a run, do two invocations.
- *Single-threaded only.* A program that spawns a thread (via the `program` construct or
  `thread<T>`) is rejected - in-process JIT'd workers would need Windows SEH unwind tables the
  JIT cannot register. Compile to an exe instead. Importing `thread.cb` without spawning is fine.
- *C interop.* Importing a real C source file (`import "file.c";`) is supported and its object is
  loaded into the in-process JIT. Prebuilt C libraries (`--c-lib`, an inline `lib` clause, or a
  package import that resolves to a library) are not supported by `--run`; use an AOT build with
  `-o` instead.
- *Entry signature.* `main` must be exactly `int main()` or `int main(int argc, char** argv)`.
- *Arguments require the matching entry.* Passing arguments after `--` to an `int main()`
  program is an error rather than a silent drop. Arguments after `--` without `--run` are also
  an error.

## Optimization and target

| Switch | Short | Value | Description |
|--------|-------|-------|-------------|
| `-O0` | `-0` | | No optimization (default). |
| `-O1` | `-1` | | Optimize for speed (level 1). |
| `-O2` | `-2` | | Optimize for speed (level 2). The loop vectorizer (and the `vectorize` keyword enforcement) runs only at `-O2`. |
| `--no-opt` | | | Disable the baseline passes (sroa, mem2reg, instcombine, simplifycfg) that run even at `-O0`. |
| `--platform` | `-p` | `win64`\|`win32`\|`linux`\|`macos` | Target platform. Defaults to the native host OS (`win64` on Windows, `macos` -> arm64 Mach-O on Apple Silicon, `linux` -> x64 ELF on Linux). `macos` cross-compiles an arm64 Mach-O object. |
| `--cpu` | | name\|`native` | Target CPU: sets ISA features and tuning. Names come from `--print-supported-cpus`. |
| `--tune` | | name\|`native` | Tune scheduling for this CPU without changing the instruction set. |

See [`doc/HPC.md`](HPC.md) for `--cpu`/`-O2` and vectorization guidance.

## Imports

| Switch | Short | Value | Description |
|--------|-------|-------|-------------|
| `--import-dir` | `-i` | dir | Directory to search for imported modules. Repeatable: pass `-i` multiple times to add several search dirs; they are searched in the order given and the first match wins. |
| `--no-runtime` | | | Do not auto-import `core/runtime.cb`. |

The `core/` directory is always on the import search path; only `runtime.cb` is auto-imported.

## C library bindings

| Switch | Value | Description |
|--------|-------|-------------|
| `--c-include` | dir | Header search directory for C library bindings (repeatable). |
| `--c-lib` | path | Prebuilt C import library (`.lib`) to link (repeatable). |
| `--c-define` | `NAME[=val]` | Preprocessor define passed to all clang-cl C compiles/dumps (repeatable). |

Full details, including the `import package` / inline `lib`/`define`/`cache` clauses, are in
[`doc/C_INTEROP.md`](C_INTEROP.md).

### macOS frameworks

| Switch | Value | Description |
|--------|-------|-------------|
| `--framework` | name | Apple framework to link as a Mach-O load command (macOS target only, repeatable). Mirrors the `import framework "name";` source form. |

Equivalent source-level forms: `import framework "AppKit";`, the brace group
`import framework { "AppKit", "Foundation" };`, and the `framework` clause on a C-header/package
import (`import package "CoreGraphics/CoreGraphics.h" framework "CoreGraphics";`) which binds the
header AND links the framework. Linking is SDK-free after a one-time `cflat --init` (harvests
AppKit / Foundation / CoreFoundation / `libobjc` tbd stubs under `~/.cflat/macsdk`); header
**binding** still needs a real SDK (`$SDKROOT` / `xcrun`). Non-macOS targets reject
`--framework` / `import framework` with a diagnostic. See [`doc/LANGUAGE.md`](LANGUAGE.md).

### vcpkg integration

| Switch | Value | Description |
|--------|-------|-------------|
| `--vcpkg-exe` | path | Explicit `vcpkg.exe` (overrides VS-bundled / `VCPKG_ROOT` / `PATH` discovery). |
| `--vcpkg-manifest` | path | Explicit `vcpkg.json` (skips the upward walk from the source file). |
| `--vcpkg-triplet` | triplet | vcpkg triplet (default derived from `--platform`: `x64-windows` / `x86-windows`). |
| `--vcpkg-no-install` | | Do not run `vcpkg install`; error out if a `package-vcpkg` port is not already installed. |

### NuGet integration

| Switch | Value | Description |
|--------|-------|-------------|
| `--nuget-packages-dir` | path | Explicit NuGet global packages folder (overrides `NUGET_PACKAGES` / `%USERPROFILE%\.nuget\packages` discovery). |
| `--nuget-no-install` | | Do not download NuGet packages; error out if a `package-nuget` package is not already in the packages folder. |

## Caching

| Switch | Description |
|--------|-------------|
| `--init` | Populate the compiler cache, then exit. Run once after installing or updating cflat. Writes to whatever `--init`'s own resolution picks (see [`doc/CACHING.md`](CACHING.md) for the 4-step order); with no local cache and no `CFLAT_CACHE_DIR` override that is the per-user cache - `%USERPROFILE%\.cflat\` on Windows (linker paths for x64/x86 + core bitcode + the compiler path for VS Code auto-detection), `~/.cflat/` on macOS/Linux (the compiler path record, the libSystem/libobjc link stubs, and core bitcode). |
| `--init-local` | Like `--init`, but populate `<exe dir>/.cflat` instead of the per-user cache, then exit. Once created, every later compile from that same exe picks it up automatically with no flag - useful for portable/xcopy installs, CI images with an unstable `$HOME`, or keeping several cflat builds (Debug/Release, multiple worktrees) from sharing one cache. Fails fast with the path and OS error if the exe directory is not writable. Also best-effort seeds `compiler_path.txt` into the per-user cache dir so the VS Code extension (which only reads `~/.cflat/compiler_path.txt`) still finds a compiler - but only when that file does not already exist, so a worktree or portable build never silently repoints an existing registration. |
| `--init-clear` | Recursively delete **both** cache directories - the per-user one (`%USERPROFILE%\.cflat\` on Windows, `~/.cflat` elsewhere) and the local one (`<exe dir>/.cflat`, if present) - then exit. Reports each root's path and how many files/bytes were removed; a missing root is reported and exits 0 (clearing the other root still happens); a partial delete (permissions) says so and exits 1. If a cache root is a symlink, only the link is removed and its target is left alone. `CFLAT_CACHE_DIR`, if set, is never deleted - only a note naming it is printed. Re-run `--init`/`--init-local` afterward - on macOS that also re-harvests the `macsdk/usr/lib` libSystem and libobjc stubs that self-contained linking depends on. Note `--init-clear` does not rebuild the `cheaders/` C-header cache; that is re-earned lazily by the next compile that imports a `cache`-clause header. Cannot be combined with `--init`, `--init-local`, or `--init-clear-local`. |
| `--init-clear-local` | Recursively delete only the local cache directory (`<exe dir>/.cflat`), then exit; leaves the per-user cache untouched. A missing local cache is reported ("no local cache at \<path\>") and exits 0 - it never falls back to clearing the per-user cache. Cannot be combined with `--init`, `--init-local`, or `--init-clear`. |
| `--no-cache` | Bypass the core bitcode cache and reparse the core libraries from source. |
| `--c-header-cache-deep` | For C headers opted in with the `cache` import clause, validate every transitively included file (mtime/hash), not just the top header. |

Cache design and troubleshooting: [`doc/CACHING.md`](CACHING.md).

## WinMD / COM

| Switch | Value | Description |
|--------|-------|-------------|
| `--emit-winmd` | path | After compiling, write the program's `[winrt]` interfaces and classes to a `.winmd` file. |
| `--dump-winmd` | path | Read a `.winmd` into the projection model and print it (diagnostic), then exit. |
| `--winmd-instantiate` | path | Import a `.winmd` and instantiate well-known parameterized interfaces (`IVector<i32>`, `IReference<i32>`, ...), checking each derived PIID + vtable shape, then exit. |
| `--winmd-sig-selftest` | | Validate the parameterized-type signature encoder + PIID derivation against published reference IIDs and the RFC 4122 v5 vector, then exit. |
| `--check` | | A `.winmd` passed to `--check` is parse-verified only (no registration). `test_winmd.bat` uses this to batch-validate every SDK `.winmd`. |

A `.winmd` brought in via `import "x.winmd";` registers its interfaces / structs / enums as CFlat
types (consume), and parameterized interfaces (`IVector<int>`, `IReference<T>`, ...) are instantiated
on demand with a derived PIID; `iidof(T)` exposes that IID. A `.winmd` passed to `--check` is
parse-verified only (no registration). Authoring COM objects with `[winrt] class`, the HRESULT /
`HResult<T>` ABI, consuming and emitting metadata: [`doc/WINMD.md`](WINMD.md).

## Diagnostics and debugging

| Switch | Short | Value | Description |
|--------|-------|-------|-------------|
| `--debug-info` | `-g` | | Emit DWARF debug information. |
| `--asan` | | | Instrument with AddressSanitizer and link the asan runtime (pair with `-g` for source-line reports). Alias: `-fsanitize=address`. |
| `--sanitize=ownership` | | | Debug-only ownership sanitizer: trap on a dereference of a moved-from owning pointer. Implies `-g`. Aliases: `-fsanitize=ownership`, `--fsanitize=ownership`. See [below](#ownership-sanitizer). |
| `--heap-audit` | | | Instrument the program with the HeapAudit leak oracle (no source edits). Requires `-o`. See [below](#heap-audit). |
| `--verbose` | `-v` | | Print detailed diagnostic messages during compilation. |
| `--locale` | | locale | Select the compiler diagnostic locale, named after a catalog in `--locale-dir` (`de`, `en-simple`, `es`, `fr`, `it`, `ja`, `ko`, `ru`, `zh-Hans`, `zh-Hant`). Defaults to `en-simple`; `CFLAT_LOCALE` is used when omitted. Use `pseudo` to print source templates and coverage warnings. |
| `--locale-dir` | | directory | Directory containing diagnostic JSON catalogs. Defaults to `<compiler directory>/locales`. |
| `--update-locale` | | locale | During compilation, collect encountered diagnostic templates and update `<locale>.json` under `--locale-dir`, preserving non-empty translations and adding source-template stubs with numbered placeholders. Generated `en-pseudo.json` also records an `argumentExamples` array for each key. Only diagnostics the run actually triggers are collected - see the catalog workflow below. |
| `--check` | | | Check one or more source files for errors without emitting output (batch; every positional is an independent source). |
| `--grammar` | | | Validate the grammar (parse only) of one or more sources; add `-v` for the full parse-tree rule stack. |
| `--xthread-scan` | | `1`..`3` | Cross-thread sharing scan level (default off). Reports non-atomic/unguarded struct fields shared across a thread spawn. 1=borrowed ctx, 2=+ptr handoff, 3=+any struct-ptr call arg. |
| `-ftime-trace` | | | Write a Chrome-trace JSON of the compile to `<input>.time-trace.json` (single-dash spelling is canonical; `--ftime-trace` also parses). |

Diagnostic message conventions: [`doc/DIAGNOSTIC.md`](DIAGNOSTIC.md). Threading-specific
analysis: [`doc/THREADING.md`](THREADING.md).

Localized diagnostics use JSON catalogs beside the compiler. The English message template
remains in compiler source and is the fallback; the default catalog is `en-simple`. Catalog keys are generated by lowercasing the
English template, replacing `{}` arguments with `arg0`, `arg1`, and removing non-alphanumeric
characters. Keys longer than 40 characters are compacted to the first 20 characters, `...`, the
last 20 characters, and a 16-digit hash of the full key. Missing or invalid translations fall
back to the source template. `--locale pseudo`
prints source templates and reports whether each `en-simple` key is present. The test
discovery pass writes the source-template catalog to `en-pseudo.json`; this is separate
from the default `en-simple.json` catalog.

Catalogs are authored under `cflat/locales/` and deployed next to the compiler by the
build and by the release packaging scripts. Keeping `en-pseudo.json` complete takes two
passes, in this order - the compiler observes real argument values but only for
diagnostics a test provokes, and the extractor statically covers every call site:

```bash
x64/Release/cflat --locale pseudo --update-locale en-pseudo --locale-dir cflat/locales \
    --check Test/errors/err_*.cb -i Test/library
python3 utilities/extract_diagnostics.py --report
```

Authoring rules for diagnostic call sites, catalog key derivation, and how to add a
language are in [`doc/DIAGNOSTIC.md`](DIAGNOSTIC.md#diagnostic-localization---authoring-and-translation).

**Language server.** `cflat lsp` picks its catalog from the editor's UI language, sent as
`initialize.params.locale` (the VS Code extension also passes it in
`initializationOptions`). A BCP-47 tag is resolved to a catalog by exact name, then by
script for Chinese (`zh-CN` and `zh-SG` to `zh-Hans`; `zh-TW`, `zh-HK` and `zh-MO` to
`zh-Hant`), then by primary subtag, falling back to `en-simple`. `CFLAT_LOCALE` overrides
the editor language when it is set in the environment the editor was launched from.

### Ownership sanitizer

`--sanitize=ownership` instruments the program for **use-after-move** - the bug class
AddressSanitizer structurally cannot see, because after a move the object is still valid,
addressable memory. It is debug-only and off by default; a build without the flag is
byte-identical to a normal build.

```bash
cflat.exe app.cb -i lib --sanitize=ownership -o app.exe
app.exe
# ownership violation: value moved at 40:9, dereferenced after move at 55:5
```

How it works: `move x` and `delete x` already null the source storage, and that null
survives being copied into a field or a container slot. The sanitizer guards the three
dereference lowerings (`p->f`, `*p`, `p[i]`) with a null check, so a use-after-move or
use-after-free is caught through any provenance - locals, struct fields, container
elements. For an owning local the compiler also keeps a hidden move-origin slot, which
turns the report into "moved at L:C, dereferenced after move at L:C"; other provenances
get a generic null-dereference message.

Behavior and limits:

- **A null *comparison* never traps.** `p == nullptr` after `move p` is legal CFlat and is
  asserted by existing tests; only a dereference is instrumented.
- **`?.` is excluded.** Null-conditional access legitimately tolerates null, so it is not
  guarded.
- **Loop-carried use-after-move is already a compile error**, caught statically by the move
  dataflow pass. The runtime guard only ever fires on the explicit-`move` form the language
  permits.
- **No double-free / use-after-drop via a non-null dangling alias.** Detecting an alias that
  still points at freed memory needs allocator-level tracking; use `--asan` for that.
- Implies `-g` so traps carry source locations. The trap shim is
  `core/diagnostic/own_sanitize.cb`, linked only under the flag.

### Heap audit

`--heap-audit` instruments a program with the HeapAudit leak oracle without
editing its source. The compiler auto-imports `core/diagnostic/heap_audit.cb`, calls
`HeapAudit.enable()` at the top of `main`, and calls `HeapAudit.reportLeaks()` before every
`return` from `main`. It is the no-source-edit equivalent of importing the module and wiring
those calls by hand.

```bash
cflat.exe app.cb -i lib --heap-audit -o app.exe
app.exe   # runs normally; the audit prints to stderr
```

Behavior:

- **Leaks are report-only.** Allocations still live when `main` returns are printed to stderr
  as `*** cflat heap-audit: LEAK ptr=... size=... ***`; the program's exit code is unchanged.
  Note that globals and singletons intended to live for the whole process show up here - this
  is a debug oracle, not a proof of leak-freedom.
- **No double-free detection.** The audit reports leaks only. Because `operator new`/`delete`
  share the CRT heap with raw `malloc`/`free`, a freed address reused by a raw allocator and
  later operator-deleted is indistinguishable from a real double free, so this oracle does not
  attempt to flag double frees. Use `--asan`, which proves a real double-free/use-after-free
  deterministically.
- **Requires `-o`.** The oracle links a C diagnostic object, so it cannot be used with `--run`
  or with IR-only output; the compiler errors out if `-o` is missing.
- **Self-auditing programs are left untouched.** If the program already calls
  `HeapAudit.enable()` or `HeapAudit.reportLeaks()` itself, no instrumentation is injected, so
  its own (typically narrower, quiescent-point) audit assertions are not perturbed.

Only allocations made *after* `enable()` that flow through `operator new`/`operator delete`
are tracked; see `core/diagnostic/heap_audit.cb` for the oracle's honest limits.

## Symbol lookup (`--symbol`)

| Switch | Value | Description |
|--------|-------|-------------|
| `--symbol` | name | IDE-style quick symbol lookup, then exit (repeatable). |

`--symbol` is an editor-style API search over the compiler's own symbol index - the *same*
index that powers LSP hover, completion, and go-to-definition. It gives a scripted or agent
caller (which has no editor) the discovery path a human gets from one.

For each `--symbol` term:

- **Exact match** (case-insensitive) prints the symbol's kind, signature, source location,
  and doc comment. For a type, namespace, or interface it also lists every member with its
  signature. Methods and fields are addressable directly, e.g. `--symbol "dictionary.set"`.
- **No match** falls back to substring and edit-distance matching and prints the closest
  symbols as "did you mean" suggestions (with their kind and location).

**What gets indexed** depends on whether you pass a source file:

- *With a positional source file* (`cflat app.cb --symbol foo`) the search covers exactly
  what that file imports - true IDE semantics, results scoped to your program.
- *With no source file* (`cflat --symbol foo`) the compiler synthesizes an import-all-core
  file, so the entire standard library is searchable with zero setup.

```bash
# Look up a container type and a specific method (whole stdlib, no project needed)
cflat.exe --symbol list --symbol "dictionary.set"

# Scope the search to one program's imports
cflat.exe app.cb -i lib --symbol Math
```

## Symbol dump (`--symbol-dump`)

| Switch | Value | Description |
|--------|-------|-------------|
| `--symbol-dump` | selector | Dump symbol info for source elements, then exit (repeatable). Requires a positional source file. |

Selectors are:

- `line:<n>` dumps one 1-based source line, for example `--symbol-dump line:49`.
- `line:<a>-<b>` dumps an inclusive line range, for example `--symbol-dump line:41-49`.
- `function:<name>` dumps the definition's function body, for example `--symbol-dump function:main`.

Line dumps ignore comments and string or character literals; unresolved identifiers produce no
symbol detail. Function dumps read the source file containing the indexed definition, including
definitions from imported files.

## Informational (print and exit)

| Switch | Short | Description |
|--------|-------|-------------|
| `--help` | `-h` | Print usage and exit. |
| `--version` | | Print the compiler version and exit. |
| `--print-supported-cpus` | | List target CPUs supported on Windows x86/x64, then exit. |
| `--print-host-cpu` | | Print the LLVM name of the host CPU (what `--cpu native` resolves to), then exit. |

## Examples

```bash
# Compile to a native exe, also dumping IR
cflat.exe app.cb -i lib -o app.exe --out-lli app.ll

# JIT-compile and run at -O2, passing program arguments
cflat.exe app.cb -i lib --run -O2 -- input.txt --flag

# Batch-check several files for errors, no output emitted
cflat.exe a.cb b.cb c.cb --check

# Bind a prebuilt C library (header + import lib)
cflat.exe app.cb --c-include C:\libs\curl\include --c-lib C:\libs\curl\lib\libcurl.lib -o app.exe
```
