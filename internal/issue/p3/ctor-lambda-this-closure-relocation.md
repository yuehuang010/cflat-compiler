# T515 residue: by-value returns remain refused

T515 item 1 is resolved in this worktree: a holder with a by-value field whose constructor stores a `this`-capturing closure now receives native sret construction. The holder can be initialized, used, and destroyed; copying the holder remains refused like copying the nested type.

T515 item 2 copy gaps are resolved: generic list members that copy closure-bearing elements are deferred until called, and round 3 refuses by-value local copies from alias/element accesses, including copies made from the borrowed range-for binding. A list can be declared, populated by move, read, iterated, counted, and destroyed; alias binding and in-place element calls remain accepted.

## Remaining gaps

- By-value returns of closure-bearing native values remain refused, for both fresh prvalues (`return S(4)`) and named locals (`return a`). They need a dedicated in-place return ABI path.

## Fix direction

Obtain the return-ABI ruling, then investigate sret-style returns for native closure-bearing results. Do not reuse the T345 r2b-r5 ABI attempt without resolving its ownership regressions.

Explicit moves such as `S b = move a` and `take(move a)` keep the closure pointing at moved-from storage by ruling. Preserve that behavior.

## Implementor-order gap (DISCFLAKE review, 2026-10-31, master too)
A copy through a generic-interface parameter `IG<S>` is not refused when the implementing instance `Box<S>` is first
instantiated AFTER the function taking the parameter (EnumerateInterfaceImplementors only sees instances registered so
far; the one allow=true consumer is the this-closure copy check, LLVMBackend_WinRT.cpp ~5282). A missed refusal, never
a miscompile. Fix direction: defer that site's enumeration to module end like the other this-closure decisions.

Note 2026-10-31: the return-ABI ruling this file waits on was given 2026-10-25 (self-pointer-struct-return-not-in-place.md: every ctor-
or dtor-bearing native struct returns via sret into the caller's slot, C++ guaranteed elision, plan first). Closure-bearing holders fall
under it; this item is buildable once that plan exists and is not a separate ruling.
