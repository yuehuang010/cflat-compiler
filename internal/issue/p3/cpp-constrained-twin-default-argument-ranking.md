# C++ constrained twins with defaults share an MSVC linkage name

## Summary

The Itanium cases for constrained member twins with default arguments are fixed and covered
by T22 legs in `Test/test_cpp_interop.cb`. The remaining known gap is MSVC ABI behavior:
clang-cl gives same-mangled-name constrained twins one emitted body, so the body choice can
depend on which arity the program uses. This Darwin worktree cannot compile or run those cases.

## Repro (Windows / clang-cl)

In `Test/library/cpp_t22_constrained_members.hpp`, `P<T>::pick(int = 1)` and the constrained
`P<T>::pick(int)` share one MSVC symbol. For `P<int>.pick()`, clang-cl emits the constrained
body (21) while the Itanium oracle selects the unconstrained default (11). The two-parameter
case has the same shared-symbol issue. Static twins can also be refused when their wrappers
collide. The issue repros and clang-cl command were preserved in the original issue history;
this worktree has no Windows toolchain to re-run them.

## Root cause

MSVC does not encode the requires-clause in the member linkage name. Wrapper registration and
demand extraction therefore cannot distinguish two bodies by linkage name alone, while
Itanium gives each constrained declaration a separate symbol.

## Fix direction

On Windows, retain the per-arity candidate domain and select the body from the used arity;
detect and report combinations that clang-cl itself rejects for duplicate mangled definitions.
Add Windows-only regression coverage before closing this issue.
