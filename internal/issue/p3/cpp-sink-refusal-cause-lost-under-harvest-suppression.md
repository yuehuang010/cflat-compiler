# C++ refusal causes are only partly surfaced

Updated 2026-09-24 on `fix/cpp-sink-refusal-cause` (macOS arm64, Release).

## Landed

- L1: refusals with a stored cause now append its first clang error line. Absolute diagnostic paths are reduced to `basename:line`. `vector.assign(n, l)` now reports `overload resolution selected deleted operator '='`.
- L2: header cache serialization caps each member's refusal cause at 4 KB, ending on a line boundary when possible. Cache version is 94 at both read validation and write, with v94 history entries.

## Remaining: M1 harvest-time cause capture

Measured before the change, `ConstOnly<Key>.add(l)` and `std.set<Key>.insert(l)` have no reliably associated per-function cause; they show the generic body refusal and ownership/move refusal respectively. Three capture/mapping attempts did not associate `std.set<Key>.insert(l)`'s harvest diagnostics with its refused overload. Each attempt left only the generic allocator construction diagnostic in the member cause. The changes were reverted per the three-attempt stop rule. Simply enabling unsuppressed diagnostics remains unsafe: clang marks the interpreter as failed, it drops the module, and later codegen dereferences null.

Do not capture a diagnostic for a function unless its location can be reliably mapped to that function. `map.insert(pair lvalue)` also still has no stored cause to append; the only printed clang clause is `no matching member function for call to 'insert'`.

## Remaining: L3

Class-name matching can blame a member copy as a parameter copy. Resolving that requires clang source-location attribution.

## Verification note

`./test.sh Release` on this host reported 1,110 passed, 1 failed, 8 skipped. The only failure was `test_cpp_interop.warm` reparsing `cpp_interop_basic.h` during the parallel suite. The isolated cold/warm sequence passed. `test_example.sh` passed 45/0.
