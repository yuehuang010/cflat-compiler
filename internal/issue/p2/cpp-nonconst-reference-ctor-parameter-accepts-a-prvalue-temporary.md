# A C++ constructor parameter declared `T&` (non-const) silently accepts a prvalue temporary

Found 2026-09-17 while fixing internal/issue/p1/cpp-constructor-reference-parameter-binds-to-a-temporary-copy.md (fix/cpp-ctor-ref-param). Pre-existing.

## Summary

C++ rejects binding a non-const lvalue reference to a temporary. CFlat accepts `Taker(make_cnt())` where `Taker(Cnt&)`: the temporary is materialized into a frame-lifetime alloca and passed by address. No longer a bitwise copy after the fix, but still no diagnostic.

## Root cause

`TypeAndValue` carries no const bit (const is dropped everywhere, see memory ruling), so a bound structor parameter cannot distinguish `T&` from `const T&`. Distinguishing them needs an extractor + registration field and a header disk cache payload bump.

## Fix direction

Record `IsConstRef` on the extracted parameter (CClangExtract -> RegisterCSignatures), bump the cache version, and reject a prvalue at a non-const reference parameter with a LogError. Keep `const T&` from prvalue working (materialize).

Suggested bucket: p2.
