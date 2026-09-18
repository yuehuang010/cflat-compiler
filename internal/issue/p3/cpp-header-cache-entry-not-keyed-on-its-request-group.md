p3

# A C++ header disk-cache entry is validated only against its OWN mtime+hash, so a sibling header in the same request group can poison it permanently

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

## Fix direction

Fold the identity of the whole request group into the validity key of every entry written from that
group: hash the group's header list together with each member's mtime+content hash, store it beside
`cxxRequestKey`, and require it to match on load. Keyless (non-request) entries produced inside a
multi-header group need it most - those are the ones with nothing else to invalidate them.

Verify with the repro above: break a namespace brace in `cpp_interop_basic.h`, compile
`Test/test_cpp_interop_template.cb` to write the entries, repair the header, then run
`x64/Release/cflat --check -i Test/library Test/errors/err_cpp_template_no_match.cb`. It must pass.
