# `--isolated` compiler policy and LLVM module validation

## Status

IMPLEMENTED 2026-08-17: all stages plus an independent review-fix round, macOS-verified
(suite 707 passed / 0 failed / 8 skipped; scratch matrices 17/17, s3, s4 all green). Stage P,
1, 2, 4, and 3 landed in that order. Enforced today: all eight capability denies via the
sealed positive symbol table (355 classified core externs) and per-module import tags;
user-extern/interop/int-to-pointer/`program` construct rejections in both passes (int-to-ptr
is semantic, with an IR inttoptr audit backstop); closed-world address-taken module audit
(incl. global initializers, aliases, llvm.used, module asm) with intrinsic allowlist and
ctor/dtor coverage; never-allowed symbol set (dlopen/dlsym/dlclose/LoadLibraryA/
GetProcAddress/syscall) rejected under every policy; native macOS arm64 `-o` behind a Mach-O
post-link audit that publishes the executable only after the audit passes (other targets:
policy-output-unsupported); `--isolated-manifest` sidecar with SHA-256 digests;
compiler-instrumented `heap_bytes`/`heap_mb` cap and `max_threads` permits via IR-level
interposition of malloc/calloc/realloc/free/posix_memalign/pthread_create/CreateThread, with the module
re-verified after instrumentation. `--isolated` is supported on macOS and Windows hosts (17+1
policy tests pass through test.bat's probe path); Linux remains gated. `-o` remains macOS arm64
only because no PE post-link audit exists. CloseHandle is Pure because it only releases an
audited handle; fwrite/fread are Stdio because read/write on an already-open FILE* follows
handle provenance, while opening remains Filesystem-gated.
Test support: `.cb.flags` sidecar convention in test.sh (flags= / expect_exit= /
expect_output=), 17+1 policy tests in Test/errors/policy/ including positive compile fixtures
and the review's bypass-shape regressions (cold+warm), pseudo-locale discovery coverage, and
scratch matrices in scratch/isolated|s3-check|s4-check.

Known limitations / remaining work: `stdio: deny` rejects most real programs at the module
audit because the always-linked runtime's panic paths reference stdio externs (strict-and-
honest; a leaner panic route would be a future refinement). test.bat has no sidecar support
yet (Windows port is maintainer-owned; policy tests live in Test/errors/policy/ which its
non-recursive glob does not pick up). Import gating under a warm --init cache is sidestepped:
--isolated forces a cold core parse. Post-link audits for cross-compiled targets are not
implemented. Duplicate-JSON-key rejection is SAX-based and done. An independent review round
(2026-08-17) fixed 13 findings, notably: heap release polarity, global-initializer address
escapes, semantic int-to-pointer rejection with an IR inttoptr backstop, never-allowed
dynamic-loading/syscall symbols, audit-before-publish for -o, post-instrumentation module
verification, and positive policy fixtures. The Windows host gate was subsequently lifted after enforcement verification; Linux remains
gated pending the equivalent host verification.

Original design intent below, revised 2026-08-17 after review (flat capability map replaces
groups; sealed-export inventory its own stage; vtable dispatch in v1 scope via the
closed-world audit; cache/LSP/testing-harness realities added).

`--isolated` verifies that a CFlat program fits a restricted language profile and that the
optimized LLVM module uses only an approved set of runtime capabilities. It preserves the
compiler's normal output choices and can later emit a manifest sidecar for an external runner.

It is not an OS sandbox, does not enforce a filesystem, network, UI, process, wall-time, or total
memory restriction at runtime, and must never be described as a security boundary. An external
runner is responsible for all runtime isolation and resource enforcement. This repository's role
ends at producing an auditable, policy-validated compiler output and reporting the capabilities it
needs when requested.

This is still useful as the first defense for a Compiler Explorer-style service. CFlat has language
and LLVM knowledge that an operating-system sandbox does not: it can reject unwanted constructs at
their source locations, remove unavailable runtime APIs from code generation, and reject a final
module that has escaped the restricted ABI. The service must separately contain hostile source
during compilation and contain the resulting output during execution.

## Goals

- Add a strict, versioned `--isolated` policy mode.
- Validate restricted CFlat semantics before code generation.
- Audit the optimized LLVM module: declarations, globals, constructors, intrinsics, call targets.
- Optionally emit a versioned manifest sidecar for an external runner (later stage).
- Preserve ordinary `--run` unchanged for trusted local development.
- Produce clear `LogError` diagnostics rather than silently deleting features or falling back.

## Non-goals

- Implement an OS sandbox, process launcher, broker, jail, container, VM, privilege model, or
  platform-specific enforcement backend.
- Claim that source or LLVM validation alone prevents malicious native behavior.
- Make the existing in-process `--run` safe for untrusted code.
- Permit arbitrary native C, header binding, prebuilt native libraries, or host symbol lookup.
- Add a new unrestricted capability merely because a policy names it.

## User interface

`--isolated` takes a policy path and selects the restricted compiler profile:

```text
cflat app.cb --isolated policy.json --check
cflat app.cb --isolated policy.json --out-lli app.ll
cflat app.cb --isolated policy.json --bitcode app.bc
cflat app.cb --isolated policy.json -o app                      # later stage: needs post-link audit
cflat app.cb --isolated policy.json --bitcode app.bc --isolated-manifest app.manifest.json   # later stage
```

`--isolated` is a compilation modifier, not a new output mode. It reuses the normal `--check`,
`--out-lli`/`-l`, `--bitcode`/`-b`, and `-o` output behavior, including their existing path and
overwrite rules. With `--check`, it validates without producing output; that is the compile-only
endpoint shape for a service.

Rejected combinations (each with a diagnostic naming the conflict):

- `--isolated` + `--run`: `--run` remains an unrestricted in-process execution path.
- `--isolated` + `.c` positional inputs, `import` of `.c`/`.h` files, `import package` (vcpkg,
  NuGet, WinMD), or prebuilt library binding: native interop is outside the restricted profile.
- `--isolated` + `--heap-audit` or `--asan`: both link extra native objects/runtimes outside the
  sealed surface.
- `--isolated` + `-o` until the target-specific post-link audit exists for the selected target:
  reject with `policy-output-unsupported`; never emit an executable on the LLVM audit alone.
- `--isolated-manifest` without `--isolated`, and (when the manifest lands) `--isolated-manifest`
  with a multi-file `--check` batch: one manifest describes one module.

Flags register in `main.cpp` (the `args.addOption`/`args.addFlag` block; `ArgParser.h` is the
generic parser and should not need changes).

## Trust and handoff model

```text
untrusted CFlat source
        |
        v
cflat --isolated policy.json [normal output flags]
  1. semantic capability validation (both passes)
  2. restricted code generation
  3. optimized LLVM module audit
        |
        v
ordinary requested compiler output (+ later: optional digest-bound manifest sidecar)
        |
        v
external runner: independently enforces OS policy
```

Neither an output nor its manifest asserts that it is safe. The runner must treat both as untrusted
input and must not infer OS permissions from `allow` entries. For a hosted service, compilation
itself also consumes hostile input before compiler checks finish; the operator must put the
compiler in a separate compile containment boundary. That deployment decision is outside this plan.

## Policy schema

Policies are UTF-8 JSON objects with closed keys at every level. Unknown keys and wrong value
types are `policy-invalid`. There is no group mechanism: the primitive namespace is closed and
small, so decisions are written directly. A service that wants named bundles of capabilities can
expand them client-side when generating the policy; a future schema version can add groups
compatibly if a real need appears.

```json
{
  "version": 1,
  "language": "restricted-v1",
  "capabilities": {
    "stdio": "allow",
    "clock": "deny",
    "random": "deny",
    "filesystem": "deny",
    "network": "deny",
    "ui": "deny",
    "process": "deny",
    "threads": "deny"
  },
  "limits": {
    "heap_mb": 192,
    "max_threads": 1
  }
}
```

### Version 1 fields

- `version`: required integer, currently `1`.
- `language`: required string, currently `"restricted-v1"`.
- `capabilities`: required object. All eight keys are required and each value is exactly
  `"allow"` or `"deny"`. Requiring every key explicitly avoids a default-direction dispute and
  keeps a policy self-describing; there is no resolution algorithm, no ordering rule, and no
  provenance machinery to specify or test.
- `limits`: optional object holding only the compiler-enforced limits:
  - `heap_bytes` or `heap_mb` (at most one): compiler-instrumented restricted-heap cap (later
    stage). `heap_mb` is mebibytes.
  - `max_threads`: concurrent CFlat thread budget including the main thread, enforced through the
    sealed thread wrapper (later stage). Must be `1` when `threads` is `"deny"`; may exceed 1 only
    when `threads` is `"allow"`.

  Runner-owned limits (total memory, wall time, process count, task count, stdout/stderr bytes)
  are deliberately NOT in this schema. The compiler cannot enforce them, and validating numbers it
  cannot enforce is fake authority; they belong in the runner's own configuration. If a service
  wants them co-located with the policy file it can nest them under its own top-level file that
  embeds this policy.

Numeric values are positive base-10 integers; zero, negative, floating-point, string, and
overflowed values are `policy-invalid`.

Note on strict parsing: nlohmann::json (already used in this codebase) keeps the last duplicate
object key rather than erroring. Duplicate-key rejection therefore needs a SAX-callback parse or
equivalent; it is required for the final version and may be deferred in the prototype with a
comment marking the gap.

`"allow"` means only that compiler validation may approve the matching sealed runtime API. It is
not a runtime grant: allowing `network` says the module may require a compiler-owned network API;
it does not permit a socket, hostname, port, or path on the host. The external runner maps allowed
capabilities to an enforceable OS policy or rejects the request.

## Sealed runtime export inventory (the load-bearing stage)

This is the largest single work item in the plan and is scheduled as its own stage, not a bullet.
The core library declares roughly 370 externs (`os.posix.cb` ~118, `os.windows.cb` ~104,
`cruntime.cb` ~90, plus `math`, `atomic`, `network/socket*`, `filesystem`, `process`, `thread`),
and `runtime.cb` is auto-imported into every program, so strings/collections bottom out in
`cruntime` externs. The final model requires the transitive extern closure of every allowed core
module to be classified.

Mechanism decision: classification lives in compiler-owned C++ tables, not in `.cb` annotations.

1. **Per-symbol capability table** (authoritative for the module audit): a static table in a new
   `IsolatedPolicy.cpp` mapping extern symbol name -> `IsolatedCapability`:

   ```cpp
   enum class IsolatedCapability {
       Pure, Stdio, Clock, Random, Filesystem, Network, Ui, Process, Threads
   };
   ```

   `Pure` covers deterministic computation (memcpy, math, allocator internals). The final model is
   a positive allowlist: an external symbol absent from the table rejects the module. The
   prototype narrows this (see Stage P) because building the complete table IS the inventory work.

2. **Per-module capability tag** (for early, source-located diagnostics): a small table mapping
   core module paths (`filesystem.cb`, `network/socket.cb`, `process.cb`, ...) to the capability
   they primarily expose, checked at import processing so a denied import errors at the `import`
   statement instead of surfacing later as an opaque module-audit failure. Mixed modules (`os.cb`
   and friends carry filesystem + process + environment externs) are NOT force-classified; the
   per-symbol audit is the backstop that actually holds.

Do not infer access from imported filename text of user modules; user modules are subject to the
semantic rules (no user externs, etc.), and only compiler-owned metadata classifies core modules.
`GetForCurrentProcess()`-style broad process-symbol search must never make a module pass isolated
validation.

The table controls semantic availability, codegen declarations, final-module validation, and later
sidecar emission. A capability module must not smuggle a second capability: a clock export must
not open a file.

### Interaction with the `--init` bitcode cache (load-bearing)

Core libraries normally load as precompiled bitcode from the warm cache, not from source, and the
repo rule stands: any state an analysis reads must round-trip through the hand-written serializer
in `LLVMBackend.cpp` or it silently vanishes on a warm cache. Consequences:

- The per-symbol and per-module capability tables are static C++ data keyed on names, so they do
  NOT depend on the cache and need no serializer entries. Prefer keeping it that way: any future
  per-declaration capability metadata stored on `TypeAndValue`/`StructData` MUST be added to the
  cache round-trip in the same change.
- Cached core bitcode was compiled without any policy. That is acceptable input to the module
  audit because the audit runs on the final linked module, where denied-capability externs are
  visible regardless of how they arrived. State this in code comments so nobody "fixes" it.
- Import-level gating must fire on the cached-core path too (imports are still resolved by name
  even when bodies come from bitcode). The prototype may instead force `--no-cache` semantics
  under `--isolated` for simplicity; if so, print nothing extra, just take the slower path, and
  remove the restriction when gating is verified on the warm path.

## Restricted CFlat profile

The profile is a positive, closed-world language subset. Validation operates on semantic facts and
generated module properties, never source-text keyword matching. Collect all independent
violations where practical, then fail the compilation via `LogError`.

Reject in restricted-v1:

- user-declared `extern` functions, globals, and aliases in user source files (core library files
  under `runtimeDir` are exempt: their externs are governed by the sealed table). This closes the
  trivial bypass of declaring `extern long syscall(...)` under a fresh name;
- `.c` inputs and imports, header binding, WinMD, NuGet or package-native imports, prebuilt native
  libraries, dynamic loading, inline assembly, and user-selected linker inputs;
- raw pointer-to-integer and integer-to-pointer conversions that can manufacture an address
  outside a CFlat allocation; any other address-manufacturing primitive found during inventory;
- `program`, process creation, shell/open-URL helpers, and direct bindings to process APIs;
- thread constructs when `threads` is denied. When allowed, the existing `thread<T>` language
  construct remains the surface syntax and is LOWERED onto the sealed spawn/join wrapper; users do
  not call a new API. (Later stage.)
- imports of core modules whose tagged capability is denied;
- unresolved externals, unknown LLVM intrinsics, generated inline assembly, and calls that cannot
  be mapped to a sealed-table entry or a defined CFlat function.

Interface dispatch is IN scope for v1: the closed-world address-escape property rejects external
function addresses entering data or indirect-call paths, while defined CFlat targets remain valid.
A v1 that cannot compile `interface` would reject most real CFlat programs and fail the Compiler
Explorer use case; this is not per-site target-set validation.

Restricting imported C is essential: the compiler cannot derive the behavior of arbitrary C
objects. A future native interop mode would require a separate reviewed ABI and object-level
audit; it is not a switch in restricted-v1.

## Compiler policy validation

Implement validation in both compiler passes. `ForwardRefScanner` must reject forbidden
declarations and imports early enough to avoid pre-registering unsupported forward references.
`MainListener` must repeat the same checks before it emits code. The two-pass rule applies: a
check added to one `ParseDeclarationSpecifiers()` copy alone is incorrect.

Policy validation creates an `IsolatedPolicyContext` (owned by `LLVMBackend`, populated in
`Compile()` before imports run) recording:

- the parsed policy: eight explicit capability decisions plus normalized limits;
- every requested capability and its source locations;
- sealed runtime exports selected by the program;
- violations, so diagnostics identify the denied rule and location.

`ResetForReanalysis` must clear the context along with all other transient per-call state (the
`lastCallIsBonded` lesson). `--isolated` is not reachable from the LSP path; assert or ignore the
context there rather than letting a stale policy leak into editor analysis.

## Compiler-instrumented memory and thread controls (later stage)

`heap_bytes`: all language heap allocation routes through a compiler-owned `IsolatedHeap` wrapper
with checked, atomic reservation accounting. Exceeding the cap reports a runtime error and aborts.
The accounting excludes allocator overhead, stacks, globals, and host runtime memory.

`max_threads`: spawn reserves a live-thread permit (main thread consumes one); join/reap releases
it. No detached threads, implicit worker pools, or unbounded task queues.

Runtime error reporting is NOT `LogError` -- `LogError` is a compile-time facility and does not
exist inside the compiled program. Specify the runtime side separately: message format
(`isolated runtime error: restricted heap cap exceeded`), stream (stderr), and exit code, in the
sealed wrapper implementation. The `resource-limit` diagnostic category below covers only the
compile-time rejections (e.g. `max_threads` contradiction in the policy).

## Optimized LLVM module validation

Run a mandatory audit after all code generation and the selected optimization pipeline, before
writing any output. Source validation is not sufficient: optimization, lowering, generic
instantiation, and compiler mistakes can change the final module. The hook point is
`LLVMBackend::Compile` after `OptimizeModule`/`RunGlobalDCE`, before the `--out-lli` /
`--bitcode` writes and before `EmitExecutable`.

The audit inspects the entire module: every function declaration and body, global, alias, named
metadata, global ctor/dtor entry, and module flag. It rejects on:

- an external function or global absent from the sealed runtime table (final model), or present
  but classified under a denied capability;
- inline assembly in a module, function, or call site;
- an LLVM intrinsic not in a small explicit allowlist justified by generated CFlat code;
- a call, `invoke`, or callbr whose target is neither a defined CFlat function nor an approved
  runtime export;
- a global constructor or destructor that reaches a denied capability or unapproved external;
- writable-and-executable section requests or equivalent properties if exposed by the output.

Build the call graph rooted at `main`, global ctors/dtors, and every address-taken function
reachable from a global or approved runtime callback; resolve defined functions transitively.

Validate with LLVM's verifier before and after the isolated audit. The verifier confirms IR
well-formedness; the isolated audit decides policy conformance. Neither proves memory safety.

`-o` needs one additional stage: the linker can introduce imports, dependencies, constructors, and
section permissions not present in the LLVM module. Until a target-specific post-link reader
audits the final executable, reject `--isolated ... -o` with `policy-output-unsupported`.

## Optional external runner handoff (later stage)

`--isolated-manifest <path>` writes a canonical JSON sidecar binding the normal output with a
SHA-256 digest (`llvm/Support/SHA256.h`). Sketch of the payload (details finalized when the stage
lands): format/version, compiler build id, LLVM major, target triple, data layout, policy digest,
`output.kind` (`llvm-ir` | `bitcode` | `executable` | `check`) and `output.sha256` (omitted for
`check`), the eight resolved capability decisions, the required-capability subset, selected
runtime exports, limits, and per-limit enforcement labels (`compiler-instrumented` vs
`external-runner-required`). Runtime exports may carry `"capability": "pure"`; `pure` is an
internal classification that policies cannot name, but the manifest reports it so a runner sees
the full export list. The sidecar is descriptive and integrity-checkable, not an authorization
token; the digest is not a signature.

## Diagnostics

Use stable categories and `LogError`:

- `policy-invalid`: unreadable/invalid JSON, unknown key, missing required key, wrong type,
  invalid number, unsupported version/language, or contradictory limits.
- `policy-unsupported`: the policy is valid but requests something this compiler build cannot yet
  enforce (e.g. denying a capability whose enforcement stage has not landed). Refusing loudly is
  mandatory; silently accepting an unenforced deny would misrepresent the output.
- `policy-capability-denied`: source requires a capability denied by the policy (reported at the
  import or first use).
- `policy-restricted-language`: a CFlat construct or import is not available in restricted-v1
  (user externs, `.c` interop, inline asm, address manufacture, `program`, ...).
- `policy-module-denied`: final LLVM module contains an unapproved external, intrinsic, call path,
  constructor, or output property; identifies the LLVM symbol and the capability or rule.
- `policy-output-unsupported`: the requested output needs an audit not implemented for this
  target (currently any `-o`).
- `resource-limit`: compile-time policy limit contradiction (runtime cap hits are reported by the
  sealed runtime wrappers, not this category).
- `isolated-manifest-error`: sidecar serialization, hashing, or writing failed (later stage).

Every policy diagnostic includes the policy filename and, where the JSON parser provides one, a
location. Source-level denials include the CFlat source location. Do not describe any diagnostic
as a sandbox or OS denial.

## Implementation stages

### Stage P: prototype -- filesystem and network enforcement (CURRENT)

Scope: a working `--isolated` that parses the v1 policy and enforces `filesystem` and `network`
denial end to end, with the honest `policy-unsupported` refusal for everything not yet enforced.

1. New `cflat/IsolatedPolicy.h/.cpp` (add to the CMake source list if not globbed): policy struct,
   nlohmann-based strict parse (closed keys, all eight capability keys required, `"allow"`/
   `"deny"` values only; duplicate-key rejection deferred with a marked gap), and the capability
   tables below.
2. `--isolated <path>` flag in `main.cpp`; wire the parsed policy into `LLVMBackend` before
   `Compile`. Reject the flag combinations listed in "User interface" (including `-o` via
   `policy-output-unsupported` and `--run`, `.c` inputs, `--asan`, `--heap-audit`).
3. Prototype enforcement rules:
   - `filesystem` / `network`: `"deny"` is enforced (below); `"allow"` is accepted and simply
     skips the denial checks.
   - the other six capabilities: `"allow"` is accepted (no enforcement claimed); `"deny"` errors
     with `policy-unsupported` naming the capability. This keeps the prototype honest: it never
     accepts a restriction it does not enforce. (Exception: `threads: "deny"` may also be
     accepted later in the prototype if the existing thread-construct rejection is trivial to
     wire; do not stretch for it.)
4. Semantic layer (both passes):
   - reject user-file `extern` declarations (`policy-restricted-language`); core files under
     `runtimeDir` are exempt;
   - reject `import` of capability-tagged core modules when denied: `filesystem.cb` ->
     filesystem; `network/socket.cb` + platform variants -> network (`policy-capability-denied`
     at the import statement);
   - reject `.c`/`.h`/package imports encountered in source (`policy-restricted-language`).
5. Module audit (in `LLVMBackend::Compile`, after optimization, before IR/bitcode writes): walk
   external declarations (functions and globals). Classify against a per-symbol table covering
   the filesystem and network externs actually declared by the core library (harvest the symbol
   lists from `filesystem.cb`, `os.cb`/`os.posix.cb`/`os.windows.cb` filesystem portions, and
   `network/socket*.cb`: `fopen`/`fread`/`fwrite`/`fclose`/`open`/`stat`/`mkdir`/`remove`/
   `rename`/`CreateFileW`/... and `socket`/`connect`/`bind`/`listen`/`accept`/`send`/`recv`/
   `getaddrinfo`/`WSA*`/...). A denied-capability symbol that survives into the final module is
   `policy-module-denied` naming the symbol. Unknown externs are NOT rejected in the prototype --
   the complete positive allowlist is Stage 2's inventory -- and combined with the user-extern
   semantic rejection this still closes the direct bypass routes for the two enforced
   capabilities. Comment this narrowing explicitly at the audit site.
6. Verification (see Testing): scratch/ repros driven manually plus the full host suite.

Acceptance: hello-world passes `--isolated policy.json --check` and `--out-lli` with fs/net
denied; a program importing `filesystem.cb` (or calling into it via an allowed import path) fails
with a located diagnostic; a socket program fails likewise; a user `extern` fails; `--run`, `.c`,
and `-o` combinations are rejected; a policy denying `ui` fails with `policy-unsupported`; the
full host suite stays green (`--isolated` off by default changes nothing).

### Stage 1: policy model completion

Strict duplicate-key rejection (SAX parse), `heap_bytes`/`heap_mb`/`max_threads` limit validation,
policy digest, and `doc/CLI.md` documentation including the non-security-boundary statement.

### Stage 2: sealed export inventory and full restricted profile

The big one. Inventory the transitive extern closure of the allowed core surface (~370 externs)
into the per-symbol table with `Pure`/`Stdio`/... classifications; flip the module audit from
"reject known-denied symbols" to "reject anything not in the table"; add the remaining
restricted-language rejections (address manufacture, `program`, process APIs); wire per-module
tags for the remaining core modules; verify import gating on the warm `--init` cache path (or
keep forcing cold parse under `--isolated` until then).

### Stage 3: restricted allocator and threads

`IsolatedHeap` routing + atomic accounting; sealed spawn/join lowering for `thread<T>`;
permit accounting; runtime error reporting spec (stderr format + exit code); flip `threads` and
heap limits from `policy-unsupported` to enforced.

### Stage 4: full module audit, `-o`, and manifest

Closed-world address-escape validation; ctor/dtor coverage; intrinsic
allowlist; target-specific post-link executable audit enabling `-o`; `--isolated-manifest`
emission with digests and enforcement labels.

## Testing strategy

Honest constraint first: the existing harnesses cannot express policy-level negatives.
`expect_error` arms from source text and `test.bat`/`test.sh` glob `Test/test_*.cb` and
`Test/errors/err_*.cb` with a fixed command line -- there is no way to pass `--isolated
policy.json` or assert a CLI-level failure today. Options, to be decided with the maintainer
before Stage 1 hardening (do not silently invent a new harness):

- extend the error-test convention with a per-test flag sidecar (e.g. `err_foo.cb.flags`), or
- add a small dedicated policy-test script alongside `test_lsp.bat`'s precedent, or
- keep policy negatives as unit-style checks inside the compiler where the repo already supports
  them.

For Stage P, verification is manual and lives in `scratch/`: a set of `.cb` repros plus policy
JSON files exercising the acceptance list above, driven by hand (or a throwaway scratch script),
plus the standard completion bar -- the full host suite (`./test.sh Release` on macOS, `test.bat`
on Windows) stays green because `--isolated` is opt-in and default-off.

Required negative coverage (accumulated across stages): invalid schema (missing key, unknown key,
bad value, bad number, wrong version/language); `policy-unsupported` for unenforced denies;
denied fs/net import and surviving symbol; user extern; `.c`/header/package interop; `--run`/
`-o`/`--asan`/`--heap-audit` combinations; later: address casts, unknown externs vs the full
table, ctor/dtor escapes, heap/thread cap boundaries, manifest tamper detection.

Required positive coverage: pure and stdio programs under `--check`/`--out-lli`/`--bitcode`;
behavior unchanged outside `--isolated`; `--run` untouched without the flag.

## Limitations and future boundary

Compiler validation lowers risk and provides an auditable contract, but a compiler, LLVM, runtime,
or generated-code defect can still produce behavior outside the model. It cannot stop raw machine
code after an exploit, constrain inherited handles, prevent system calls, or cap total process
memory. A public service must not rely on `--isolated` alone.

Ownership split for any future runner integration: this compiler owns policy parsing, restricted
semantics, LLVM audit, ordinary output emission, and sidecar accuracy; the runner owns runtime
authorization, OS containment, and resource limits; the service operator owns compile
containment, authentication, rate limits, and retention.

Do not expand restricted-v1 to permit native interop, broad host symbol lookup, or indirect calls
with unknown targets. Add a new policy version rather than weakening version 1.

## Primary API references

- [LLVM IR language reference](https://llvm.org/docs/LangRef.html)
- [LLVM verifier API](https://llvm.org/doxygen/Verifier_8h.html)
- [Bitcode format and reader/writer APIs](https://llvm.org/docs/BitCodeFormat.html)
