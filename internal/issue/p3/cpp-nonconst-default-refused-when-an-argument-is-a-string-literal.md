# C++ non-constant default argument refused when another argument is a string literal

A C++ function with a non-constant default (`const ImVec2& size = ImVec2(0, 0)`) is callable with
the default omitted - through the generated `__cflat_dflt_*` wrapper - EXCEPT when another
argument of the call is a string literal. Then:

```
call to 'dns.Lab' is missing parameter 'size'. Its default value is not a constant, so you must pass it explicitly.
```

(source string: `LLVMBackend_Overloads.cpp` ~3318, "omits parameter ... whose default argument is
not a constant expression"). Found 2026-09-26 validating the imgui spike: `ImGui.Button("press")`
fails in `scratch/imgui_spike/spike1.cb`, `spike2.cb`, `spike_hdr.cb`; all three built and ran on
2026-09-07. Not incremental-specific (`CFLAT_CPP_INCREMENTAL=0` fails the same way), not the
`.cpp` import (header-only fails too).

## Repro (in-repo, `--check` is enough, ~1 s)

```cpp
// dv3.h
#pragma once
struct DV { float x, y; constexpr DV() : x(0), y(0) {} constexpr DV(float a, float b) : x(a), y(b) {} };
namespace dns {
inline bool Lab(const char* label, const DV& size = DV(0, 0)) { return label && size.x == 0; }
inline bool LabI(int n, const DV& size = DV(0, 0)) { return n && size.x == 0; }
}
```

```cflat
import cpp "dv3.h";
extern int main()
{
    const char* s = "y";
    bool a = dns.Lab("x");          // FAILS: missing parameter 'size'
    bool b = dns.Lab(s);            // ok
    bool c = dns.LabI(1);           // ok
    bool d = dns.Lab("x", DV(1, 1)); // ok (explicit)
    return 0;
}
```

Also fine: the same default on an inline function, an out-of-line declaration, a by-value
parameter, or a global-namespace function - as long as no argument is a string literal.

## Fix direction

The call binds the ORIGINAL overload (with the unfilled nonconst default) instead of the
`__cflat_dflt_*_<arity>` wrapper when an argument is a string literal. Compare how a string
literal argument ranks against the wrapper's `const char *` parameter vs the original's in
overload resolution (`LLVMBackend_Overloads.cpp`; string-literal special cases also exist in
`RequestCxxVariadicConstructor`, `LLVMBackend_CInterop.cpp` ~9103). Candidate regression window:
the overload tie-breaker commits (8d949b04, d8d21f18) - unverified, do not bisect by commit
(reduce instead). Once a candidate needs a nonconst default and a wrapper of the matching arity
is bound, the wrapper should win.

## Notes for the fixing agent

- Regression test: extend an existing `Test/library/cpp_interop_*.h` fixture with the `Lab` shape
  and add legs to `Test/test_cpp_interop.cb` (literal, `const char*` local, explicit) - no new
  test files.
- Real-world check (local-only, `scratch/` is gitignored - skip if absent), from the repo root:
  `x64/Release/cflat scratch/imgui_spike/spike1.cb --import-dir scratch/imgui_src -o scratch/validate_0926/imgui_spike1 && ./scratch/validate_0926/imgui_spike1`
  -> exit 0 with `TotalVtxCount` > 0. The old `--cpp-assume-noexcept` flag in the spike's Sep 7
  args no longer exists; drop it (noexcept default ruling 2026-09-08).
