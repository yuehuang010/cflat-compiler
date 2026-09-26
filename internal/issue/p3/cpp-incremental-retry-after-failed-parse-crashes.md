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
