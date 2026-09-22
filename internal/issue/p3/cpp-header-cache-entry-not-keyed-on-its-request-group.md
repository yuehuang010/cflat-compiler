p3

# A C++ header disk-cache entry is validated only against its OWN mtime+hash, so a sibling header in the same request group can poison it permanently

RULED 2026-09-20, stopgap LANDED as 1cd4d31a. Measured first: "any clang error -> no store" is
too broad (clean SDK headers report intentional macro-probe errors in the stub), and in-header
errors were already refused. The actual hole was a header that leaves a scope OPEN: clang's
"expected '}'" lands in the generated stub, which the in-scope error count skips. The stub now
carries a sentinel after the includes; a sentinel outside file scope refuses the group, names the
scope, and nothing is stored. One in-memory slot remembers the last refusal (validated by stat of
every file the failed TU entered) so the LSP does not re-run clang. Pinned by
Test/errors/err_cpp_header_leaves_scope_open.cb. STILL OPEN here: a sibling / transitive edit
that changes extraction WITHOUT any error (a new `using namespace`, an added #include) - that is
the request-group / always-on dependency keying question below.

Found 2026-09-17 while root-causing a post-rebase "regression" on fix/cpp-const-ref-scalar that
turned out to be a poisoned cache entry, not a code defect. Cost one review round.

## What happened

`Test/test_cpp_interop_template.cb` and `Test/test_cpp_interop_bridge.cb` both import
`library/cpp_interop_basic.h` and `library/cpp_interop_tpl.h`, and MakeCxxRequestGroup compiles a
group's headers in ONE clang translation unit. During a window when `cpp_interop_basic.h` was
transiently brace-unbalanced (a botched merge resolution left `namespace cppi` open), that TU
swallowed tpl.h's top-level `namespace cppt` into `cppi`. The cache entry written for
`cpp_interop_tpl.h` therefore recorded its function templates as `cppi.cppt.twice` and its using
directives as `cppi.cppt -> cppi.cppt_inner`.

Repairing `cpp_interop_basic.h` did NOT invalidate that entry: TryLoadCHeaderDiskCache validates an
entry against the cache version, the header's own mtime and the header's own content hash (plus an
optional `cxxRequestKey` for request-scoped entries - the poisoned entry had none). So every later
compile replayed `cppi.cppt.*`, `HasCxxFunctionTemplate("cppt.twice")` answered false, and three
fixtures took a completely different diagnostic path:

- `Test/errors/err_cpp_template_no_match.cb` -> "'twice' is not a member of namespace 'cppt' (C++
  free function 'cppt.twice' could not be bound (clang: invalid operands ...))" instead of "no
  instantiation of C++ function template 'cppt.twice' accepts these argument types"
- `Test/errors/err_cpp_brace_arg_no_match.cb` -> "the function 'cppt.twice' is not known."
- `Test/errors/err_cpp_template_ctor_diagnostics.cb` -> the template-argument diagnostic instead of
  the "is a C++ variable, not a class template" one

Deleting the one entry made all three pass again, on the same binary.

## Why it is worth fixing

A malformed header is user error, but the staleness hole is not: any edit to header A that changes
how header B extracts inside a shared group leaves B's entry valid by B's own key. Adding a
namespace, a `using namespace`, or an `#include` to A is enough. The failure mode is silent and
survives every later build, which makes it very expensive to diagnose - it looks exactly like a
compiler regression.

## Fix direction (amended 2026-09-17 after a batch attempt measured the premise)

The filed direction does NOT fix the repro. The poisoned entry's request group is
`[cpp_interop_tpl.h]` alone, and its validity key already covers that group's mtime+content
hash; `cpp_interop_basic.h` poisons it as a TRANSITIVE `#include` of tpl.h (line 11), not as a
group member. The real hole: transitive-include dependency tracking (`entry.deps` +
`CHeaderDepFresh`) is gated behind `--c-header-cache-deep`, off by default. Measured: the
identical scenario with `--c-header-cache-deep` on both compiles passes
(scratch/bp3_cache_repro_deep.sh in the batch worktree; plain repro script bp3_cache_repro.sh).

So the fix is a POLICY decision, not a one-site edit: make transitive dependency tracking
always-on (cost: hashing every transitive include on every C/C++ header import, Windows SDK
umbrellas included - needs a perf accept-set measured on `import "windows.h" cache;`), or a
cheaper middle ground (record deps always, but validate them only by mtime, not content hash).
Needs a maintainer ruling on the cost before build.

Verify with the repro above: break a namespace brace in `cpp_interop_basic.h`, compile
`Test/test_cpp_interop_template.cb` to write the entries, repair the header, then run
`x64/Release/cflat --check -i Test/library Test/errors/err_cpp_template_no_match.cb`. It must pass
WITHOUT `--c-header-cache-deep`.
