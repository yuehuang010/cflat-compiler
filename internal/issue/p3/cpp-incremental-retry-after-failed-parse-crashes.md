# Incremental C++ retry after a failed parse can crash clang's GenModule

Bucket: p3 (latent crash; the one known trigger is closed). Found 2026-09-25 on
fix/cpp-implicit-ctor-adam (3da52cb5), analysed by a Fable advisor against the pinned LLVM 23.1.0
sources.

## Mechanism

IncrementalParser::ParseOrWrapTopLevelDecl checks hasErrorOccurred(), then runs
Local/GlobalInstantiations.perform() and HandleTranslationUnit. An error raised in that
post-check phase makes CodeGeneratorImpl::HandleTranslationUnit reset its module
(ModuleBuilder.cpp ~319) and IncrementalAction::GenModule dereferences the null module
(IncrementalAction.cpp ~116). A retry chunk whose kept declarations reference members poisoned by
the failed attempt hits this. Interpreter::Undo is not a fix (a failed parse registers no PTU).

## State

The known trigger (a member default-argument wrapper re-emitted by a later type request, then
DropBlamedDeclarations retry) is closed by 3da52cb5: every `__cflat_dflt_` name in a chunk is
looked up in the live TU and renamed, and a wrapper redefinition outside a wrapper batch is a
hard failure. Other DropBlamedDeclarations retries remain (libtorch t31 cold: 17 ordinary ones,
none crash). If one ever crashes: catch the condition before GenModule (e.g. check
hasErrorOccurred() after the parse returns and before code generation in cflat's own wrapper),
or turn that retry into a hard diagnostic naming the first clang error.

## Second trigger (closed 943ea9ab)

libtorch t14-t16 (first bad 9b825a07): a wrapper naming a protected nested type failed a type
request, and the retry's rename rebuilt bitcode from clang's per-chunk module, dropping bodies.
943ea9ab renames inside the harvested bitcode and builds no wrapper that names a non-public nested
type; guard legs 3637-3639 (cppexp::PrivLoader).

## Repro attempts (2026-11-10, a3retry; no crash, no fix)

Master exe, `CFLAT_CACHE_DIR` isolated, `-B -v`. Headers are in-repo shapes, not system headers.
None of the three entered DropBlamedDeclarations (0 "dropped declarations" lines), so none
exercised the trigger.

1. `template <class T> struct Box { T v; int plain(); int get() { return v.missing(); } int
   twice() { return get() * 2; } };` with `Box<int> b = default; b.twice()`: located error
   "member 'twice' of C++ class 'Box<int>' cannot be instantiated" (rc 1), no retry.
2. `template <class T> struct Sm { static const int k = T::nothing; int plain(); };` with
   `Sm<int> s = default; s.plain()`: "could not be instantiated: clang reported an invalid
   specialization" (rc 1), no retry.
3. `template <class T> struct Da { T v; int get(int k = T::missing()); };` with
   `Da<int> d = default; d.get()`: "clang: type 'int' cannot be used prior to '::'" (rc 1).

Retries on master are reached only from generated-code errors: test_cpp_interop.cb with -v
logs 71 "dropped declarations" lines (mostly "use of undeclared identifier 'a1'" and "no
matching constructor").

Lead, unverified: DropBlamedDeclarations (CxxIncrementalGroup.cpp ~622) drops a kept-or-blamed
segment for a failed member only when its text is `::m)`, `->m(` or `.m(`. The generated
static-variable use helper (LLVMBackend_CHeaderDemand.cpp ~2330) is `(void)&Marker::var;`, which
none of those match. A static member that fails, with its error blamed elsewhere in the chunk,
would then be kept and re-parsed against the poisoned member. Next attempt: force a retry whose
blamed segment is a sibling helper, not the static-var helper.
