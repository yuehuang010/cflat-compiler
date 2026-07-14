# Lock capability: `[Capability(ILockable)]` - source-declared lock types

**Status: LANDED (phases 1-4). Phase 5 (raw-acquire enforcement) is NOT planned - see below.**

Verified on macOS arm64 Release: `test.sh` 167 passed / 0 failed / 15 skipped; LSP suite
5/5 smoke, 46/46 fixtures, 151/152 sweep (the one sweep failure is a pre-existing skip-list
omission for `example/ui/09-map/map_app.cb`, unrelated to this work).

Removed the compiler's knowledge of *which types are locks*. Before this, the `lock()`
statement duck-typed on the method names `acquire` / `release` / `acquire_read` /
`release_read` hardcoded in `MainListener.h`. Lock-ness is now a declared property of the type
in source: no lock *type* name appears in C++, and a user-defined lock participates with no
compiler change.

Related: `internal/plan/lock-guard-mandatory.md` (why scoped `lock()` is optional and raw
acquire/release stays legal).

## What landed

A type declares its lock capability with the `[Capability(...)]` annotation, whose arguments
name interfaces in `core/interfaces.cb`. The interface supplies the *shape* (the method set);
the annotation supplies the *intent* (a compile-time capability, not a dispatchable
interface). Classes may instead use the existing nominal base list.

```cflat
// core/interfaces.cb
annotation Capability { string value; };

interface ILockable       { void acquire();      void release();      };
interface ISharedLockable : ILockable { void acquire_read(); void release_read(); };
interface ICvWaitable     { void sleep_cv(void* cvSlot); };

// core/mutex.cb, core/rwlock.cb, core/semaphore.cb  (all three gained: import "interfaces.cb";)
[Capability(ILockable, ICvWaitable)]  struct mutex     { ... };
[Capability(ISharedLockable)]         struct rwlock    { ... };
[Capability(ILockable)]               struct semaphore { ... };

// user code - participates with no compiler change
[Capability(ILockable)]               struct MySpinLock { void acquire() {...} void release() {...} };
class MyFutureLock : ILockable        { ... };   // nominal path, unchanged
```

The argument list is a union of independent capabilities, not a re-listing of an inheritance
chain: `ISharedLockable : ILockable` composes transitively, so `rwlock` names only the derived
interface.

The compiler's entire lock vocabulary is now one table in `MainListener.h` - interface names
and the method roles within them. No type names:

```cpp
struct CapabilitySpec { const char* iface; const char* acquire; const char* release; const char* mode; };
static constexpr CapabilitySpec kCapabilities[] = {
    { "ILockable",       "acquire",      "release",      ""     },
    { "ISharedLockable", "acquire_read", "release_read", "read" },
};
```

`lock(m)` resolves the receiver's type, finds its capability interfaces, and picks the
acquire/release pair whose `mode` matches the `.read` / `.write` suffix. Renaming `acquire` to
`lock` across core is now a one-row edit, not a compiler change.

### Static conformance vs dynamic conformance

Interface values are fat pointers `{vtable, data}`; vtables are side-table globals keyed per
`(type, interface)`, built lazily by `GetOrCreateVTable`. An object never carries a vptr, so
conformance costs nothing in layout for a struct *or* a class. The distinction is about
existentials, not memory:

- `[Capability(I)]`  = static conformance. Shape checked at compile time. The type can
  **never** be converted to an `I` fat pointer.
- `: I` (class)      = nominal conformance. Dispatchable as an `I` value, as before.

Both register the capability. Keeping them apart is what stops a `mutex` from being silently
boxed at an overload boundary - verified: `ILockable l = m;` reports "'mutex' does not
implement interface 'ILockable'", and a `[Capability]`-only struct emits zero vtable globals.

### Changes by file

- **`CFlat.g4`** - `annotationArg` gains `Identifier`; new `annotationArgList`; `annotation`
  takes the list; `annotationList?` added to both `structDefinition` alternatives (`struct`
  and `union`). ANTLR was clean: zero ambiguity warnings, because `declaration` and
  `classDefinition` in `aggregateMember` already lead with `annotationList?`, so that decision
  was already full-context.
- **`LLVMBackend.h`** - `AnnotationValue::Values` (all args; `Value` still holds the **first**,
  so `[uuid]` / `[JsonName]` / `[MaxLength]` / `AnnotationArgText` and the COM/WinRT readers are
  untouched). `StructData::StaticInterfaces` + `RegisterStructStaticInterfaces` /
  `GetStructStaticInterfaces` / `TypeHasCapability` (transitive via `InterfaceInheritsFrom`).
  `TypeImplementsInterface` consults `Interfaces` **or** `StaticInterfaces`; the four
  conversion sites (upcast, `is`, and the two overload-resolution sites) still read
  `Interfaces` only.
- **`LLVMBackend.cpp`** - core-bitcode-cache round-trip for `AnnotationValue::Values` and
  `StructData::StaticInterfaces`. **Not in the original plan, and required**: `typeAnnotations_`
  is not serialized and is cleared on cache load, so without this a `--init`-warmed cache
  silently dropped every `[Capability]` - a bug that only reproduces on a warm cache.
- **`MainListener.h`** - `AnnotationArgTexts` / `AnnotationArgOne`; `ExtractAnnotations` +
  `ParseAnnotationList` populate `Values`; `annotationof` comma-joins multi-arg. Struct
  annotations recorded in **both** passes (`ForwardRefScanner::ScanStructDefinition`
  pre-registers `StaticInterfaces` on the shell so a `lock()` textually above the lock type
  still classifies; `ParseStructDefinition` validates and calls `VerifyInterfaceImplementation`
  per interface once members exist). `kCapabilities` table; `lock()` lowering classifies
  through it; the four hardcoded method strings and the `release_read`->`release` fallback hack
  are gone. Interface-typed (dynamic) lock targets are rejected - the lock-set analysis
  canonicalizes targets by name and cannot name a dynamic one.
- **`core/interfaces.cb`, `core/mutex.cb`, `core/rwlock.cb`, `core/semaphore.cb`** - as above.
- **`Test/errors/err_lock_wrong_mutex.cb`** - two `expect_error` cases: a duck-typed
  `acquire`/`release` struct with no `[Capability]` is rejected by `lock()`; a
  `[Capability(ILockable)]` struct missing `release` is rejected at definition.

New diagnostics:

```
lock: type 'Plain' is not a lock type (missing [Capability(ILockable)]).
lock: type 'mutex' does not support 'read' mode (missing [Capability(ISharedLockable)]).
[Capability] on 'Weird': unknown interface 'INotAThing'
```

Side effect, wanted: `where L : ILockable` now works on a `[Capability]` struct (via
`TypeImplementsInterface`), for `mutex` and for a user-defined `MySpinLock`. That opens the
door to a generic `guarded<T, L>` / `withLock` closure form later.

## Alternatives rejected

**Hardcode `"mutex"` / `"rwlock"` in C++.** Every future lock type needs a compiler edit, and
a user-defined lock silently escapes. Explicitly ruled out by `lock-guard-mandatory.md`.

**Pure duck typing ("has acquire/release").** Cannot distinguish a lock from a page pool or
`NumaDomain`'s factory, which have `acquire` methods and are not locks.

**Role annotations on interface methods (`[acquires]` / `[releases]`).** Would keep the set of
capability interfaces open, but that openness is illusory: every new lock flavor needs new
compiler *syntax* anyway (try-lock needs `lock (m) else { }`, timed-lock needs a duration
argument), so a library-only `ITryLockable` would have no statement that could use it. Not
worth the extra annotation vocabulary.

**Allowing `struct mutex : ILockable` (base list on structs).** Simpler grammar-wise, but it
makes every lock type an existential candidate - the opposite of what a systems-level lock
wants.

### Precedent

The compiler already depends on `interfaces.cb`: `IReflector` is hardcoded by name *and by
method name* in ~20 places, emitting `CallInterfaceMethod(visitor, "IReflector", "visitInt", ...)`.
`[winrt]` / `[uuid(...)]` are ordinary annotations declared in `com.cb` and consumed by the
compiler. This work follows both patterns.

## Phase 5 (raw-acquire enforcement): NOT planned

The original plan treated `CheckRawLockCall` - rejecting `mutex.acquire()` outside a `lock()`
block - as the payoff. That was wrong. Scoped `lock()` is **optional by design**; see
`lock-guard-mandatory.md`. Raw acquire/release must stay legal for hand-over-hand lock coupling
(the planned concurrent B-tree needs FIFO release order, which LIFO scoped blocks cannot
express), for returning a held lock, and for a future try-lock.

Implementing phases 1-4 surfaced a **second, independent reason** enforcement cannot work as
specified, and it is worth recording:

**`semaphore` is legitimately both a lock and not a lock.** `lock (s) { }` on a semaphore is
existing, tested behavior (`Test/test_sync.cb:599`, `testSemaphoreLockStatement`), so
`semaphore` necessarily carries `[Capability(ILockable)]`. But a semaphore is *also* used as a
counting handoff - acquired on one thread, released on another - which is non-lexical by nature
and can never be a `lock()` block. One type, two legitimate usage modes.

This means "is a lock" and "must be taken via `lock()`" are **orthogonal properties**. The
original plan conflated them, and its claim that "`semaphore.acquire()` is untouched because
`semaphore` carries no `[Capability]`" is false. As specified, `CheckRawLockCall` would flag
every raw `sem.acquire()` in core.

If enforcement is ever revisited, it needs a **second marker** independent of `[Capability]` -
e.g. `[ScopedLock]` on `mutex` / `rwlock` but not `semaphore` - plus the hand-over-hand escape
hatch (`[ManualLocks]`, Clang's `NO_THREAD_SAFETY_ANALYSIS`). Do not attempt it without both.
