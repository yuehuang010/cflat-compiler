# No way to select a C++ standard above C++20

Found 2026-09-09 by the std header coverage spike ([`std-header-coverage-spike.md`](std-header-coverage-spike.md), gap 7).
Needs a maintainer ruling on the target standard before implementation, not just a flag.

## Repro

```cflat
import cpp "expected";
extern int main() { std.expected<int, int> e = default; return 0; }
```

    'std::expected<int, int>' does not name a C++ class type in the imported headers

The header itself binds - `import cpp "expected";` with an empty `main` passes `--check`. The MSVC
STL simply compiles the declaration out below C++23, so the name is genuinely absent.

## Root cause

`cppStandard_` is initialized to `"c++20"` (`cflat/LLVMBackend.h:3209`) and nothing ever assigns
it: `ArgParser` exposes no override, and there is no `--cpp-std` anywhere in the tree. The value
feeds `-std=` at `LLVMBackend_CInterop.cpp:393,399,1681` and
`LLVMBackend_EmitAndLink.cpp:3217`; the clang-cl path hardcodes `/std:c++20` separately at
`LLVMBackend_CInterop.cpp:481`, so a flag has to move both.

## Blocked headers

`expected`, `flat_map`, `flat_set`, `generator`, `mdspan`, `print`, `stacktrace`, `stdfloat` - all
bind at L0 and are unreachable at L1.

## Fix direction

Two decisions first: which standard cflat targets by default, and whether the standard is
per-invocation or per-import. Then a `--cpp-std` CLI flag setting `cppStandard_`, with the
clang-cl `/std:` spelling derived from it rather than hardcoded.

Acceptance: `--cpp-std c++23` makes the repro compile and run to exit 0; the default remains
whatever the ruling says, asserted in `Test/test_cpp_interop.cb`.
