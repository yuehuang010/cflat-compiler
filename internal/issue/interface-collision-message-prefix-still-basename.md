# Interface collision message: the location PREFIX is still a basename

Filed 2026-07-28 while fixing the def-site basename hole (finding 2 of the now-removed
`iface-namespace-follow-ups.md` entry). That fix made the "already defined at ..." HALF
of the message name the colliding file unambiguously (install-relative for a core site,
cwd-relative or basename-fallback for a user site). It did not touch the message's own
PREFIX - the `file(line,col):` text every `LogError` diagnostic is stamped with - which
still comes from `sourceFileName`, a bare basename. That half of the original finding is
still open.

## Repro

Two same-basename files, each declaring `interface IFoo` with a different shape,
co-imported:

```
da/common.cb:1  interface IFoo { int a(); };
db/common.cb:1  interface IFoo { i64 b(); };
```

The reported message:

```
common.cb(1,0): interface 'IFoo' is already defined at da/common.cb(1,0) - an interface
name must be unique within its namespace
```

The "defined at" clause now correctly says `da/common.cb(1,0)` (disambiguated). The
`common.cb(1,0):` prefix - naming the file where THIS occurrence of the error was
raised, i.e. `db/common.cb` - still prints as the bare basename `common.cb`, so a reader
cannot tell which of the two same-named files this prefix refers to without cross
referencing line/col by hand.

## Root cause

The prefix is stamped by the generic diagnostic-formatting path, not by
`CreateInterfaceDefinition` itself. It reads `sourceFileName`, which is set to a bare
basename alongside (but separately from) the full canonical path:
`cflat/LLVMBackend.cpp:344`, `:1559`, `:2169` set `currentSourceFilePath_` (canonical,
used for the fixed half of this bug) at the same sites where `sourceFileName` (basename,
used for the prefix) is set. The two have always been companion fields; only the
def-site identity/display path was changed by the recent fix, not the general
diagnostic prefix.

This is a broader, pre-existing convention (every compiler error is prefixed with a bare
basename, not just interface-collision ones), so fixing it here alone would be
inconsistent with every other diagnostic in the compiler. That is why it is filed
separately rather than folded into the def-site fix.

## Fix direction

Decide whether the diagnostic PREFIX convention should change compiler-wide (all
`LogError`/`LogErrorContext` call sites use `sourceFileName`), which is a much bigger
surface than this one guard. A narrower option: only for the interface-collision path,
detect when the prefix's basename collides with the def-site's basename and disambiguate
just that case (e.g. append enough of the path to distinguish it) - but that special-cases
one diagnostic away from the rest of the compiler's prefix convention, which may not be
worth the inconsistency. No decision made yet; needs product input before implementing
either direction.

---

# Cosmetic: core def-site display can go stale relative to the live install

Two related, CONFIRMED-by-reasoning (not required to reproduce to trust) staleness gaps
in the `--init` bitcode cache, discovered while verifying the def-site fix's cache
round-trip. Neither affects correctness of the duplicate-definition GUARD (which always
fires or stays silent identically cold vs. warm) - both are purely about the TEXT of a
cached core def-site going out of sync with reality.

## (a) Moved install, preserved mtimes

`--init` bakes the install's current absolute core path into
`~/.cflat`'s cached def-site strings (see `defsite`/`defsite_is_core`,
`cflat/LLVMBackend.cpp` around the cache read/write). The cache key is derived from the
mtimes of `runtimeDir/core/*.cb`, not from the install's location. Copying (or moving) an
installed compiler to a new path while preserving file mtimes leaves the warm cache keyed
correctly (mtimes unchanged) but reporting the OLD absolute path in any core-collision
message, while a cold run (cache invalidated or deleted) from the new location correctly
reports the new path. Master's basename-only def-site was location-agnostic and could
never go stale this way - this is new surface area introduced by carrying a real path
into the cached string, even though the path is only ever used for cosmetics (identity
still worked correctly).

## (b) In-place upgrade reuses a pre-fix cache

An in-place upgrade from a pre-fix build to this fix (replacing the compiler executable
without touching `core/*.cb`) does not invalidate the `--init` cache, because the cache
key is derived from core `.cb` mtimes, which an exe swap does not change. The old,
pre-fix cache holds bare-basename core def-sites. A warm run against that stale cache
displays `interfaces.cb(96,0)` (old, ambiguous form) instead of `core/interfaces.cb(96,0)`
(new, install-relative form) until the next `--init` repopulates it. Still functionally
correct (the guard still fires; only the cosmetic path format lags), but inconsistent
with a cold run immediately after the same upgrade.

## Fix direction

Both are display-only staleness, not correctness bugs, and low priority. If ever
addressed: (a) could be avoided by not baking an absolute path into the cache at all -
store a core-relative path unconditionally for entries where `defsite_is_core` is true,
computed at read time from the CURRENT `runtimeDir` instead of frozen at `--init` time.
(b) is inherent to any format change carried through a cache keyed on unrelated content
(core file mtimes) - the general fix is documenting that a compiler binary upgrade should
be followed by `--init` if diagnostic text formatting changed, which is already implicit
in "the cache is a performance optimization, not a correctness dependency" but is not
written down anywhere today.
