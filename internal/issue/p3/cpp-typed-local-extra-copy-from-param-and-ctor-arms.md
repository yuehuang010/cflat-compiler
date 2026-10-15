# Spelled-type C++ class locals make copies/moves C++ elides

Re-measured 2026-10-15 on master 614704be1 against clang++ -std=c++20 (probes and matrix:
scratch/t229_tmp/, scratch/t229_matrix.md in the fix/t229-typedlocalcopy worktree). Values and
destructor pairing are correct everywhere; only the counts below differ.

Already matching clang on master (no action): `T y = s;` from a by-value parameter where the
parameter is used afterwards (class with listed ctors, converting template ctor, and aggregate with
implicit copy ctor); `T x = move hp->s;` and `move hp->t` (local, parameter, new-held, dot, `(*hp).t`,
`get()`, CFlat struct field); `T z = *o;` / `auto z = *o;` on a C++ `operator*` returning `S&`
(plain, ref-qualified, `std.optional<S>`); `?:` arms of `T(args)` / lvalue / `ref()` in declarations.
The earlier "refused: needs a plain variable" and "copy 3" reports did not reproduce on any shape tried.

## Still differs

R1 - last use of a by-value parameter (`T y = s;` and `auto y = s;`, s not used afterwards):
clang copies (no implicit move for a declaration from a name); CFlat moves (copy 1 + move 1 with
an lvalue argument, clang copy 2 counting the argument). Pinned by Test/test_cpp_interop.cb 7340/7341
("CFlat's rule, as for the spelled type"). Typed and auto agree today. Fixing only the typed spelling
splits the spellings; fixing both breaks read-only leg 7341. NEEDS A MAINTAINER RULING: keep CFlat's
last-use move (then drop the clang claim) or match clang for both spellings (then 7341 changes).

R2 - prvalue passed to a by-value C++ class parameter (`f(S(5))`): CFlat moves the temporary into the
parameter and destroys the moved-from shell (ctor 1 move 1 dtor 2; clang ctor 1 dtor 1). General call
argument path, not a declaration. This is the remaining guaranteed-elision part of the partial
p2 item in internal/issue/Queue.md (line 92, N67). The issue file it names is not in internal/issue/;
not re-filed here.

R3 - nested `T w = T(T(6));` (and auto): the inner prvalue is copied into the outer constructor
argument (copy 1, dtor 2; clang ctor 1, dtor 1). Left unfixed (brief item 4: not trivial). Same family
as R2 on the constructor-argument path.

## Legs

None added: no shape in items 1-3 fails on the pre-fix build, so no leg would be a regression test
for a change here. Existing legs 7300-7341 (Test/test_cpp_interop.cb) keep their counts.
