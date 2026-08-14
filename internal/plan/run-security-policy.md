# `--isolated` compiler policy and LLVM module validation

## Status

Proposed design for compiler-side validation only. `--isolated` verifies that a CFlat program fits a
restricted language profile and that the optimized LLVM module uses only a sealed set of approved
runtime capabilities. It preserves the compiler's normal output choices and can optionally emit a
manifest sidecar for an external runner.

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
- Audit the optimized LLVM module, including constructors, declarations, globals, intrinsics, and
  the selected output's requirements.
- Optionally emit a versioned manifest sidecar that lets an external runner choose and enforce its
  own platform policy without rediscovering compiler intent.
- Preserve ordinary `--run` unchanged for trusted local development.
- Produce clear `LogError` diagnostics rather than silently deleting features or falling back.

## Non-goals

- Implement an OS sandbox, process launcher, broker, jail, container, VM, privilege model, or
  platform-specific enforcement backend.
- Claim that source or LLVM validation alone prevents malicious native behavior.
- Make the existing in-process `--run` safe for untrusted code.
- Permit arbitrary native C, header binding, prebuilt native libraries, or host symbol lookup in
  the first restricted profile.
- Add a new unrestricted capability merely because a policy names it.

## User interface

`--isolated` takes a policy path and selects the restricted compiler profile:

```text
cflat app.cb --isolated policy.json --check
cflat app.cb --isolated policy.json --out-lli app.ll
cflat app.cb --isolated policy.json --bitcode app.bc
cflat app.cb --isolated policy.json -o app
cflat app.cb --isolated policy.json --bitcode app.bc --isolated-manifest app.manifest.json
```

`--isolated` is a compilation modifier, not a new output mode. It reuses the normal `--check`,
`--out-lli`/`-l`, `--bitcode`/`-b`, and `-o` output behavior, including their existing path and
overwrite rules. With `--check`, it validates without producing a normal compiler output. It is
suitable for an editor or web service's compile-only endpoint. It must not add a special output
directory, payload format, or output-path rule.

`--isolated` is deliberately not an alias for `--run` and is incompatible with the current
in-process `--run`. A later external runner may consume an ordinary bitcode, IR, object, or
executable output accompanied by an optional manifest; its command-line interface and OS policy
are outside this design. Reject `--isolated` combined with `--run` with a diagnostic explaining
that `--run` remains an unrestricted in-process execution path.

`--isolated-manifest <path>` is optional. It writes a canonical JSON sidecar for the selected
ordinary output, or for the validated in-memory module with `--check`. It never changes output
selection, is not needed for validation, and does not authorize execution. A service that has its
own compiler-to-runner protocol may omit it. The compiler must not invoke an external runner,
shell, linker selected by the input, package manager, or network resolver as part of isolated
validation.

## Trust and handoff model

```text
untrusted CFlat source
        |
        v
cflat --isolated policy.json [normal output flags]
  1. semantic capability validation
  2. restricted code generation
  3. optimized LLVM module audit
        |
        v
ordinary requested compiler output (+ optional digest-bound manifest sidecar)
        |
        v
external runner: independently enforces OS policy
```

Neither an output nor its optional manifest asserts that it is safe. The manifest asserts only that
a particular compiler build successfully applied a particular policy to a particular validated
representation. The runner must treat both as untrusted input: verify sizes, hashes when present,
version compatibility, target, and policy constraints before execution. It must not infer OS
permissions from `approved` entries in the manifest.

For a hosted service, compilation itself also consumes hostile input before compiler checks finish.
The website operator must put the compiler in a separate external compile containment boundary.
That deployment decision is intentionally outside this compiler plan.

## Policy schema

Policies are UTF-8 JSON objects with closed keys at every level. Unknown keys, duplicate JSON
object keys, duplicate array entries, and duplicate group members are errors. All numeric values
are positive base-10 integers; zero, negative, floating-point, string, and overflowed values are
errors. Paths, environment expansion, and platform-specific access-control semantics are not part
of this schema because this compiler does not enforce them.

Example starter policy:

```json
{
  "version": 1,
  "language": "restricted-v1",
  "groups": {
    "interactive": ["cap:stdio", "group:workers"],
    "workers": ["cap:threads"],
    "host-access": ["cap:filesystem", "cap:network", "cap:ui", "cap:process"]
  },
  "capabilities": {
    "allow": ["group:interactive"],
    "deny": ["group:host-access", "cap:random", "cap:clock"]
  },
  "limits": {
    "heap_mb": 192,
    "memory_mb": 256,
    "wall_time_ms": 5000,
    "max_processes": 1,
    "max_threads": 4,
    "max_tasks": 4,
    "stdout_bytes": 65536,
    "stderr_bytes": 65536
  }
}
```

### Version 1 fields

- `version`: required integer, currently `1`.
- `language`: required string, currently `"restricted-v1"`.
- `groups`: required object mapping a user-defined group name to a nonempty array of typed member
  references. A group name matches `[a-z][a-z0-9-]{0,31}`. It is always referenced as
  `group:<name>`, so it cannot be confused with a built-in capability or another JSON field.
- `capabilities`: required object with exactly the two keys `allow` and `deny`; each is an array of
  typed capability/group references. The arrays may be empty.
- `limits`: required object with positive integer entries described below.

The built-in primitive capability namespace is closed in version 1: `stdio`, `clock`, `random`,
`filesystem`, `network`, `ui`, `process`, and `threads`. A primitive is referenced only as
`cap:<name>` (for example, `cap:network`); a group is referenced only as `group:<name>`. No policy
can define a new primitive capability, alias an unknown primitive, or use an untyped reference.
`pure` remains an internal compiler classification, not a policy capability and therefore cannot be
named in this schema.

`allow` means only that compiler validation may approve the matching sealed runtime API. It is not
a runtime grant. For example, allowing `cap:network` says that the module may require a
compiler-owned network API; it does not permit a socket, hostname, port, filesystem path, UI
service, or subprocess on the host. The external runner must map the resolved primitive capabilities
to an enforceable runtime policy or reject the request.

### Group resolution and decisions

The parser validates all group definitions, including unused groups, before compilation. A member
reference must be exactly `cap:<built-in-name>` or `group:<defined-group-name>`. A group can refer
to a group declared before or after it. Self-reference and indirect cycles are invalid. Version 1
caps policy complexity at 64 groups, 64 members per group, 1,024 total group-member references, 128
references in each decision array, and a maximum group-reference depth of 16. Exceeding any cap is
`policy-invalid`, rather than a partial expansion.

Resolution expands every decision reference to a set of primitive capabilities. It always uses these
fixed phases, in the stated order:

1. Initialize every primitive capability to `deny`.
2. Expand and apply every entry in `capabilities.allow`, setting each reached primitive to `allow`.
3. Expand and apply every entry in `capabilities.deny`, setting each reached primitive to `deny`.

The deny phase is a final veto. Allow/deny overlap is intentional and valid, including a group or
the same `cap:<name>` in both arrays; a primitive reached by deny cannot be re-allowed by this
policy version. Direct primitive references have no special precedence over groups. The same typed
reference may not appear twice in one array, but it may appear once in each array. Declaration
order within either array never changes the result.

The normalizer produces a canonical, lexically ordered final decision for all eight primitives
before semantic validation, code generation, or LLVM auditing. It also records JSON Pointer paths
for each allow and final-deny contribution, so diagnostics can explain an effective decision.
Group names and decision arrays never reach later validation as authority; only this final primitive
decision map does.

For example, this policy allows `stdio` and `threads`, denies every host-access capability, and
defaults any omitted primitive to deny:

```json
{
  "version": 1,
  "language": "restricted-v1",
  "groups": {
    "standard-io": ["cap:stdio"],
    "parallel": ["cap:threads"],
    "interactive": ["group:standard-io", "group:parallel"],
    "host-access": ["cap:filesystem", "cap:network", "cap:ui", "cap:process"]
  },
  "capabilities": {
    "allow": ["group:interactive"],
    "deny": ["group:host-access", "cap:random", "cap:clock"]
  },
  "limits": {
    "heap_mb": 192,
    "memory_mb": 256,
    "wall_time_ms": 5000,
    "max_processes": 1,
    "max_threads": 4,
    "max_tasks": 4,
    "stdout_bytes": 65536,
    "stderr_bytes": 65536
  }
}
```

The deliberate allow/deny overlap below resolves `network` to deny because the final deny phase
wins:

```json
"capabilities": {
  "allow": ["group:host-access"],
  "deny": ["cap:network"]
}
```

Likewise, a group may appear in both arrays. This is a useful policy convention for broad baseline
allows followed by explicit deny groups; it is deterministic and does not depend on JSON order.

### Declared limits

- `heap_bytes` or `heap_mb`, exactly one: a compiler-instrumented limit on allocations charged to
  the restricted CFlat heap. `heap_mb` is normalized as mebibytes (1 MiB = 1,048,576 bytes).
- `memory_bytes` or `memory_mb`, exactly one: declared whole-workload memory ceiling for the
  external runner. It covers code, stacks, globals, runtime and allocator overhead, and memory the
  compiler cannot observe. The compiler checks only syntax, normalization, and
  `heap_bytes <= memory_bytes`; it cannot enforce this cap.
- `wall_time_ms`: declared wall-clock ceiling for the external runner. It is recorded but not
  enforced by the compiler.
- `max_processes`: declared process ceiling. It must be `1` in restricted-v1 because process
  creation is always rejected.
- `max_threads`: concurrent CFlat thread budget, including the main thread. It is enforced through
  the restricted runtime wrapper when threads are allowed.
- `max_tasks`: declared total task ceiling for an external runner. It must be at least
  `max_threads` and at least `max_processes`.
- `stdout_bytes` and `stderr_bytes`: declared output ceilings for an external runner. CFlat's
  restricted output helpers can account for their writes, but direct descriptor writes are not a
  compiler-enforceable boundary, so an emitted manifest marks these as runner-enforced.

When resolved `threads` is `deny`, `max_threads` must be `1`. When it is `allow`, `max_threads` may
be greater than one. `max_tasks` defaults to `max_threads` only if an explicit default is
introduced; version 1 requires it to prevent accidental ambiguity.

## Restricted CFlat profile

The profile is a positive, closed-world language subset. Validation operates on semantic facts and
generated module properties, never source-text keyword matching. Collect all independent violations
where practical, then use `LogError` for the compilation failure.

Reject in restricted-v1:

- user-declared `extern` functions, globals, aliases, and externally supplied function pointers;
- `.c` inputs and imports, header binding, WinMD, NuGet or package-native imports, prebuilt native
  libraries, dynamic loading, inline assembly, and user-selected linker inputs;
- raw pointer-to-integer and integer-to-pointer conversions that can manufacture an address outside
  a CFlat allocation; any other unsafe address-manufacturing primitive found during inventory;
- `program`, process creation, shell/open-URL helpers, and direct bindings to process APIs;
- thread types, thread pools, task runtimes, detached threads, and implicit worker pools when
  `threads` is denied; when allowed, any path other than sealed spawn/join runtime APIs;
- imports of filesystem, network, UI, clock, random, or terminal APIs whose capability is denied;
- runtime or generic features that allocate without the restricted allocator when `heap_bytes` is
  enabled;
- unresolved externals, unknown LLVM intrinsics, generated inline assembly, and direct calls that
  cannot be mapped to a sealed manifest entry or a defined CFlat function.

Restricting imported C is essential to the first release: the compiler cannot reliably derive the
behavior of arbitrary C objects or their constructors. A future native interop mode would require a
separate reviewed ABI, an object-level audit, and an external runner policy; it is not a switch in
restricted-v1.

### Capability-owned runtime exports

Every restricted runtime external must have one central declaration. The conceptual inventory is:

```cpp
enum class IsolatedCapability {
    Pure, Stdio, Clock, Random, Filesystem, Network, Ui, Process, Threads
};

struct IsolatedRuntimeExport {
    std::string_view symbol;
    IsolatedCapability capability;
    bool usesRestrictedHeap;
    bool createsThread;
};
```

The table is a positive allowlist, not a name blacklist. It controls semantic availability, codegen
declarations, final-module validation, and optional sidecar emission. Each runtime module imports
only the entries it needs. `GetForCurrentProcess()` and broad process-symbol search must never be
used to make a module pass isolated validation.

The initial default surface should contain deterministic computation, strings, collections, bounded
stdio, arguments, and a small allocation API. Clock, random, files, network, UI, process, and
threads are opt-in runtime modules. A capability module must not smuggle a second capability; for
example, a clock export must not open a file, and an output export must not launch a UI program.

## Compiler policy validation

Implement validation in both compiler passes where type and declaration information is consumed.
`ForwardRefScanner` must reject forbidden declarations and imports early enough to avoid
pre-registering unsupported forward references. `MainListener` must repeat the same declaration
specifier and construct checks before it emits code. This repository's two-pass rule applies to all
new type parsing and soft keyword handling.

Policy validation should create an `IsolatedPolicyContext` carried through import processing,
semantic analysis, and code generation. It records:

- normalized limits, the canonical final decision for every primitive capability, provenance for
  the allow and deny expansions, and a canonical JSON digest;
- every requested capability and its source locations;
- restricted runtime exports selected by the program;
- allocation and thread constructs that require instrumentation;
- policy violations, so diagnostics can identify the denied rule and location.

Do not infer access from imported filename text. Standard-library modules must declare their required
capability in compiler-owned metadata. User modules remain subject to the same semantic rules. An
import of a denied module produces `policy-capability-denied` at the import and, when available, at
the first use.

Before code generation, reject capability contradictions: a denied capability with any selected
export, a process count other than one, `threads: "deny"` with thread constructs, or a heap-routed
operation with no approved allocator. Check both `ParseDeclarationSpecifiers()` implementations in
`MainListener.h`; a change to one pass alone is incorrect.

## Compiler-instrumented memory and thread controls

### Restricted heap

`heap_bytes` has deterministic CFlat semantics. In isolated mode, all language heap allocation,
resize, and collection growth must route through a compiler-owned `IsolatedHeap` API. It atomically
reserves requested usable bytes before calling its backing allocator, releases the recorded usable
size on free, and reserves only the growth delta on realloc. Overflow, double free, or an allocation
past the limit reports `resource-limit: restricted heap cap exceeded` and aborts the program.

The accounting definition deliberately excludes allocator overhead, stacks, globals, JIT code,
mappings, and host runtime memory. Those are accounted only by the external runner's declared
`memory_bytes` limit. Before enabling this feature, inventory and redirect CFlat `new`, arrays,
strings, closures, generic collections, exceptions if supported, and runtime-owned temporary
buffers. The final module audit rejects calls to an allocator outside the sealed allocation exports.

The implementation must use checked size and alignment arithmetic plus atomic reservation so
allowed concurrent threads cannot oversubscribe the cap. This wrapper makes memory failures
predictable for CFlat code; it does not contain arbitrary native code.

### Threads

When `max_threads` is one, semantic validation rejects every thread construct and thread runtime
export. When it is greater than one, expose only `cflat_isolated_thread_spawn` and
`cflat_isolated_thread_join`. Spawn reserves a live-thread permit before creating a thread; join or
reap releases it. The main thread consumes one permit. Exceeding the budget reports
`resource-limit: max_threads exceeded` at the spawn call.

No detached threads, implicit runtime worker pools, or unbounded task queues are permitted in
restricted-v1. A future task library may multiplex logical tasks on the permitted threads only with
bounded queues charged to the restricted heap. LLVM materialization and compiler-owned support work
must not silently consume program permits.

The compiler can enforce the language path and instrument its own wrapper, but it cannot stop a
memory-corruption defect from reaching a native thread primitive. An emitted manifest must
therefore mark `max_threads` as `compiler-instrumented` and require an external runner to treat
`max_tasks` as the hard runtime ceiling.

## Optimized LLVM module validation

Run a mandatory audit after all CFlat code generation and the selected optimization pipeline, before
writing `--out-lli` or `--bitcode` output. Source validation is not sufficient: optimization,
lowering, generic instantiation, runtime declarations, and compiler mistakes can change the final
module.

The audit must inspect the entire module, including every function body, declaration, global,
alias, named metadata, global constructor/destructor entry, and module flag. It rejects the module
on any of the following:

- an external function, global, alias, or object requirement absent from the sealed runtime table;
- an approved external whose required capability is denied by the policy;
- a call, `invoke`, call-br instruction, indirect-call provenance, or function address that escapes
  the set of defined CFlat functions plus approved runtime exports;
- inline assembly in a module, function, or call site;
- an LLVM intrinsic not in a small explicit allowlist justified by generated CFlat code;
- allocator, thread, process, file, network, UI, or dynamic-loader symbol outside its approved
  restricted runtime entry;
- a global constructor or destructor that reaches a denied capability, an unapproved external, or a
  forbidden allocator/thread API;
- an object-file input, library, target feature, data layout, target triple, or module flag not
  accepted by the selected isolated output format;
- writable-and-executable section requests or equivalent code-generation properties if exposed by
  the selected output format.

Direct symbol checks are necessary but not enough. Build a call graph rooted at `main`, global
constructors, global destructors, and every address-taken function reachable from a global or
approved runtime callback. Resolve defined functions transitively. For an indirect call, require a
provable finite target set contained in that graph; otherwise reject it in restricted-v1. This may
initially exclude flexible callbacks and virtual dispatch patterns until the compiler can preserve
and validate their target sets correctly.

Validate LLVM IR with LLVM's verifier before and after the isolated audit. The verifier confirms IR
well-formedness; the isolated audit decides policy conformance. Neither proves memory safety or
runtime isolation.

### Output-specific validation

`--check`, `--out-lli`/`-l`, and `--bitcode`/`-b` validate the optimized LLVM module immediately
before reporting success or writing the selected output. LLVM IR and bitcode are the preferred
handoff representations in the initial implementation because the audited module remains directly
inspectable by a runner.

`-o` must remain a supported ordinary output choice, but it needs one additional validation stage.
The linker may introduce imports, library dependencies, constructors, section permissions, and
other executable properties not present in the optimized LLVM module. Before `--isolated ... -o`
can report success, the compiler must inspect the final linked executable using a target-specific
reader and reject imports, load commands/dependencies, entry initializers, sections, and metadata
outside the restricted output allowlist. If that post-link audit is not implemented for the selected
target, reject this combination with `policy-output-unsupported`; do not silently emit an executable
based only on the LLVM audit. This is staged support, not a new packaging format.

The policy never controls output paths, bundled libraries, response files, or embedded objects.
Existing output flags retain their ordinary path behavior. Size caps for an output are configuration
of the caller or eventual runner, not a security property of this mode.

## Optional external runner handoff

When requested with `--isolated-manifest <path>`, the compiler writes a canonical JSON sidecar. It
is optional for every output choice and is never required for compilation or validation. For
`--out-lli`, `--bitcode`, or a post-link-audited `-o` output, it binds the normal output file with a
SHA-256 digest. For `--check`, it records the policy and validated module metadata but has no output
file digest. Its `output.kind` is respectively `llvm-ir`, `bitcode`, `executable`, or `check`, and
`output.sha256` is omitted for `check`. It does not create a directory, bundle files, or establish
a new output format.

The sidecar contains at least:

```json
{
  "format": "cflat-isolated-manifest",
  "format_version": 1,
  "compiler_build_id": "...",
  "llvm_major": 0,
  "target_triple": "...",
  "data_layout": "...",
  "policy_version": 1,
  "policy_sha256": "...",
  "output": {
    "kind": "bitcode",
    "sha256": "..."
  },
  "resolved_capabilities": {
    "clock": "deny",
    "filesystem": "deny",
    "network": "deny",
    "process": "deny",
    "random": "deny",
    "stdio": "allow",
    "threads": "allow",
    "ui": "deny"
  },
  "required_capabilities": ["stdio", "threads"],
  "capability_provenance": {
    "stdio": { "allow": ["/capabilities/allow/0 -> group:interactive"] },
    "threads": { "allow": ["/capabilities/allow/0 -> group:interactive"] },
    "network": { "allow": ["/capabilities/allow/0 -> group:host-access"], "deny": ["/capabilities/deny/1"] }
  },
  "runtime_exports": [
    { "symbol": "cflat_isolated_alloc", "capability": "pure" },
    { "symbol": "cflat_isolated_thread_spawn", "capability": "threads" }
  ],
  "limits": {
    "heap_bytes": 201326592,
    "memory_bytes": 268435456,
    "wall_time_ms": 5000,
    "max_processes": 1,
    "max_threads": 4,
    "max_tasks": 4,
    "stdout_bytes": 65536,
    "stderr_bytes": 65536
  },
  "enforcement": {
    "heap_bytes": "compiler-instrumented",
    "max_threads": "compiler-instrumented",
    "memory_bytes": "external-runner-required",
    "wall_time_ms": "external-runner-required",
    "max_processes": "external-runner-required",
    "max_tasks": "external-runner-required",
    "stdout_bytes": "external-runner-required",
    "stderr_bytes": "external-runner-required",
    "filesystem": "external-runner-required",
    "network": "external-runner-required",
    "ui": "external-runner-required"
  }
}
```

`resolved_capabilities` is the complete normalized primitive decision map and is required. The
optional `capability_provenance` is diagnostic metadata only; it may identify policy JSON Pointer
paths and group expansions, but no runner may re-expand groups or reapply `allow`/`deny` entries.
A runner consumes the resolved primitives and its own independently supported profile, or rejects
the handoff.

The external runner must reject a normal output and sidecar when their format, compiler/LLVM
compatibility rules, target, data layout, policy hash, output digest when present, required
capability set, or resource requirements do not fit the runner's own supported profile. The runner
must resolve only `runtime_exports` that it has independently approved, and must apply its OS policy
before executing code or constructors.

The sidecar is descriptive and integrity-checkable, not an authorization token. A service should
either keep outputs private to the request or bind them to its own authenticated job record. Do not
treat the SHA-256 digest as a signature or permit a user to replace the sidecar with one for another
output.

## Diagnostics

Use stable categories and `LogError`:

- `policy-invalid`: invalid JSON, unknown key, duplicate, invalid typed reference, unknown
  primitive/group, invalid group name, group cycle, expansion limit, invalid number, unsupported
  policy version, or contradictory limits.
- `policy-capability-denied`: source requires a capability denied by the policy.
- `policy-restricted-language`: a CFlat construct or import is not available in restricted-v1.
- `policy-runtime-denied`: selected runtime export is not in the sealed table or is denied.
- `policy-module-denied`: final LLVM module contains an unapproved external, intrinsic, call path,
  constructor, object requirement, or output property.
- `policy-output-unsupported`: the requested output needs a target-specific isolated audit that is
  not implemented for this target.
- `resource-limit`: deterministic restricted heap or thread permit limit was exceeded.
- `isolated-manifest-error`: optional sidecar serialization, hashing, or writing failed.

Every policy diagnostic includes the policy filename, line/column when the JSON parser can provide
one, and an RFC 6901 JSON Pointer (for example `/groups/interactive/1` or
`/capabilities/deny/0`). A source-level denial includes the CFlat source location plus the policy
pointer(s) that caused the resolved deny; an IR-only failure identifies the LLVM function/global
and the relevant resolved capability or sealed-table rule. Do not describe any diagnostic as a
sandbox or OS denial.

## Implementation stages

### Stage 1: policy model and CLI

1. Add strict JSON parsing, typed group-reference validation, graph-cycle/size validation,
   fixed-phase capability normalization, canonical serialization, and policy digest.
2. Add `--isolated <policy>` and optional `--isolated-manifest <path>` validation in `ArgParser.h`
   and `main.cpp`; retain the existing `--check`, `--out-lli`/`-l`, `--bitcode`/`-b`, and `-o`
   output flags and explicitly reject the combination with `--run`.
3. Define `IsolatedPolicyContext`, capability metadata, and stable error categories.
4. Document the mode's non-security-boundary limitation in CLI help and `doc/CLI.md`.

Acceptance: malformed, incomplete, unknown-version, unknown-key, duplicate, unknown reference,
cyclic or oversized group graph, overflowed, and contradictory policies fail before compilation;
the final eight-capability decision map is fixed before semantic checks; existing output selection
remains unchanged; `--run` behavior remains unchanged.

### Stage 2: restricted semantic profile

1. Add checks in both `ForwardRefScanner` and `MainListener`, including both
   `ParseDeclarationSpecifiers()` implementations where relevant.
2. Reject native interop, forbidden imports, externs, raw address manufacture, process APIs, and
   disallowed threads before code generation.
3. Replace broad runtime availability with the central sealed export table.
4. Record source-level capability requests against the normalized primitive decisions for optional
   sidecar emission.

Acceptance: violations identify the restricted rule/capability and source location; permitted
programs select only their required compiler-owned runtime exports.

### Stage 3: restricted allocator and threads

1. Inventory every allocation route and redirect isolated code to `IsolatedHeap`.
2. Add checked atomic heap accounting, allocation-site diagnostics, and final-module allocator
   validation.
3. Add sealed spawn/join exports and live-thread permits; reject detached and implicit thread paths.
4. Mark compiler-instrumented and external-runner-required limits accurately in an emitted sidecar.

Acceptance: heap and thread boundaries give deterministic CFlat diagnostics; no generated module
can bypass the sealed allocator or thread exports.

### Stage 4: final module audit and output support

1. Run LLVM verification and the isolated audit after optimization.
2. Implement call-graph and indirect-call target validation, constructor/destructor coverage, and
   intrinsic/external checks.
3. Gate `--check`, `--out-lli`/`-l`, and `--bitcode`/`-b` on the optimized-module audit.
4. Add a target-specific final executable/import audit before allowing `-o`, initially rejecting
   unsupported targets with `policy-output-unsupported`.
5. Emit an optional canonical manifest sidecar with output hashes, resolved primitive decisions,
   and optional non-authoritative group provenance when there is an output file.

Acceptance: an unapproved external declaration, constructor call, or target/data-layout field makes
the module audit fail deterministically; a linked executable import or dependency outside the
allowlist makes the post-link audit fail; an unsupported target cannot emit an isolated executable.
When requested, an output or sidecar hash mismatch is detectable by a consumer.

## Testing strategy

Extend existing related test files and scripts; do not add new compiler integration test files unless
explicitly requested. Add focused unit coverage where the repository already supports it.

Required negative cases:

- invalid policy schema, unknown fields, duplicate keys/references, invalid group-name syntax,
  untyped or unknown references, unknown primitive capability, self-reference, indirect cycle,
  depth/member/total expansion limits, numeric overflow, and limit contradictions;
- baseline default-deny behavior; group expansion; allow then deny final-veto behavior for direct
  capabilities and groups; same group in both arrays; overlapping groups; order-independent output;
  and a resolved decision map containing all eight primitives before source or IR validation;
- user `extern`, `.c` input/import, header/package binding, prebuilt library, dynamic loader, and
  inline assembly rejection;
- denied filesystem, network, UI, process, clock, random, stdio, and thread module/import usage;
- raw address casts and unsupported indirect-call targets;
- forbidden calls from global constructors/destructors and address-taken callbacks;
- unknown external, global, alias, intrinsic, allocator, thread primitive, target feature, and
  object requirement in a final module;
- one byte below, at, and above the restricted heap cap; overflow and realloc growth/shrink; racing
  allocations near the cap;
- `max_threads` at one and N, N concurrent permitted spawns, N+1 rejection, and permit reuse after
  join;
- IR and bitcode output is blocked when the optimized module audit fails;
- executable output is blocked when final-link import/dependency/section audit fails, and rejected
  with `policy-output-unsupported` where that audit has not been implemented;
- optional sidecar output hash, target, data-layout, policy digest, resolved capability map,
  runtime export, enforcement label, and non-authoritative group provenance tamper detection by a
  consumer-side test helper; consumer coverage must prove that it never reinterprets groups.

Required positive cases:

- pure restricted program compiles under `--check`, `--out-lli`, and `--bitcode` with no optional
  sidecar;
- a group may reference a group declared later and resolves to the same primitive set regardless of
  declaration order;
- allowed stdio and an allowed bounded thread computation compile with exact required exports;
- optional sidecars describe `--check`, IR, bitcode, and post-link-audited executable outputs with
  complete resolved primitive capabilities, without changing their normal output paths;
- program behavior and source locations are unchanged outside `--isolated` mode;
- ordinary `--run` remains available only without `--isolated`.

The full host suite remains the completion bar after compiler changes. No test in this plan asserts
OS denial, process containment, or total memory enforcement because those properties belong to the
external runner.

## Limitations and future boundary

Compiler validation lowers risk and provides an auditable contract, but a compiler, LLVM, runtime,
or generated-code defect can still produce behavior outside the model. It cannot stop raw machine
code after an exploit, constrain inherited handles, prevent system calls, cap total process memory,
or reliably contain threads. A public service must not rely on `--isolated` alone.

Any future external runner integration must remain a separate design with a clear ownership split:

- this compiler owns policy parsing, restricted CFlat semantics, LLVM audit, ordinary output
  emission, and optional sidecar accuracy;
- the runner owns runtime authorization, OS containment, total memory/time/output/task limits,
  file/network/UI/process enforcement, and output admission;
- the service operator owns compile containment, request authentication, rate limits, queues,
  output retention, and deployment patching.

Do not expand restricted-v1 to permit native interop, broad host symbol lookup, indirect calls with
unknown targets, or a feature the compiler cannot trace to a sealed runtime export. A future policy
version should be added rather than weakening version 1.

## Primary API references

LLVM:

- [LLVM IR language reference](https://llvm.org/docs/LangRef.html)
- [LLVM verifier API](https://llvm.org/doxygen/Verifier_8h.html)
- [ORC design and `JITDylib::define`](https://llvm.org/docs/ORCv2.html)
- [Bitcode format and reader/writer APIs](https://llvm.org/docs/BitCodeFormat.html)
