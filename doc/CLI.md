# Command-Line Reference

Every switch accepted by `cflat.exe`, grouped by purpose. This page is the human-readable
companion to `cflat --help`; both derive from the flag registrations in
[`cflat/main.cpp`](../cflat/main.cpp), so when a switch changes, update both.

```
cflat <input> [options]
cflat --init | --version | --help | --print-supported-cpus | --print-host-cpu
cflat lsp                       # run the Language Server (see the VS Code extension)
cflat lsp --lsp-pool-size N      # cap concurrent analyses to N backends/workers
```

The `lsp` subcommand accepts `--verbose`/`-v`, `--import-dir`/`-i <dir>`, and
`--lsp-pool-size <N>`. The pool size bounds how many analyses run in parallel (one
backend and worker thread per slot); it defaults to the logical CPU count. The switch
takes precedence over the `CFLAT_LSP_POOL_SIZE` environment variable.

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
| `--nologo` | | | Hide the version banner and the trailing "Done." line. Useful for scripting. |

You can combine `-o` with `-l`/`-b` to emit the exe and the IR/bitcode in one pass.

## Execution (`--run`)

| Switch | Value | Description |
|--------|-------|-------------|
| `--run` | | JIT-compile and run the program in-process - no exe on disk. The process exit code is the program's exit code. |

`--run` is the fastest edit-compile-run loop: no linking, no temp `.exe`, no sibling DLL
copies. The full `-O2` pipeline (if requested) runs before execution.

**Entry point.** `--run` looks up a function named `main`, which must be one of:

```cflat
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
| `--platform` | `-p` | `win64`\|`win32` | Target platform (default `win64`). |
| `--cpu` | | name\|`native` | Target CPU: sets ISA features and tuning. Names come from `--print-supported-cpus`. |
| `--tune` | | name\|`native` | Tune scheduling for this CPU without changing the instruction set. |

See [`doc/HPC.md`](HPC.md) for `--cpu`/`-O2` and vectorization guidance.

## Imports

| Switch | Short | Value | Description |
|--------|-------|-------|-------------|
| `--import-dir` | `-i` | dir | Directory to search for imported modules. |
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

### vcpkg integration

| Switch | Value | Description |
|--------|-------|-------------|
| `--vcpkg-exe` | path | Explicit `vcpkg.exe` (overrides VS-bundled / `VCPKG_ROOT` / `PATH` discovery). |
| `--vcpkg-manifest` | path | Explicit `vcpkg.json` (skips the upward walk from the source file). |
| `--vcpkg-triplet` | triplet | vcpkg triplet (default derived from `--platform`: `x64-windows` / `x86-windows`). |
| `--vcpkg-no-install` | | Do not run `vcpkg install`; error out if a `package-vcpkg` port is not already installed. |

## Caching

| Switch | Description |
|--------|-------------|
| `--init` | Populate `%USERPROFILE%\.cflat\` (linker paths for x64/x86 + core bitcode), then exit. Run once after installing or updating cflat. |
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
| `--heap-audit` | | | Instrument the program with the HeapAudit leak oracle (no source edits). Requires `-o`. See [below](#heap-audit). |
| `--verbose` | `-v` | | Print detailed diagnostic messages during compilation. |
| `--check` | | | Check one or more source files for errors without emitting output (batch; every positional is an independent source). |
| `--grammar` | | | Validate the grammar (parse only) of one or more sources; add `-v` for the full parse-tree rule stack. |
| `--xthread-scan` | | `1`..`3` | Cross-thread sharing scan level (default off). Reports non-atomic/unguarded struct fields shared across a thread spawn. 1=borrowed ctx, 2=+ptr handoff, 3=+any struct-ptr call arg. |
| `-ftime-trace` | | | Write a Chrome-trace JSON of the compile to `<input>.time-trace.json` (single-dash spelling is canonical; `--ftime-trace` also parses). |

Diagnostic message conventions: [`doc/DIAGNOSTIC.md`](DIAGNOSTIC.md). Threading-specific
analysis: [`doc/THREADING.md`](THREADING.md).

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
