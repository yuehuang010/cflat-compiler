# Doc gaps and stale comments found by an external project (v0.11.0, macOS arm64)

Filed 2026-08-21 from an external report (a quant backtester built against v0.11.0). Each item
below is a documentation/comment fix, not a code defect - grouped into one file rather than four
one-line issues. Verify each against the tree before editing; the report is from v0.11.0.

## 1. Freeing elements of a `list<T*>` is documented only in an error message

```cflat
list<Node*> xs; xs.add(new Node()); delete xs.get(0);
```
correctly gives
```
cannot delete the alias (borrowed) result of 'get': the owner still holds it, so this delete would
double-free when the owner frees it. Use an owning accessor such as take()/removeAt(), or let the
owner free it.
```
The message is good - the problem is that it is the ONLY place explaining how to free owned
pointer elements. The `list` documentation should state the rule directly: `take()`/`removeAt()`
transfer ownership, `get()`/`[]` do not, and `list<unique T*>` frees for you. Cross-reference
`list<alias T*>` (see [[delete-borrow-via-named-local]]) as the opt-in borrow spelling.

## 2. `process.cb` header comment says "x64 only (STARTUPINFOA layout)"

The POSIX path works on arm64 macOS - the reporter ran it. The comment is stale and scares readers
off a supported configuration. Reword to name the Windows-specific constraint only.

## 3. `UI.md` says Cocoa is "compile-checked; runtime verification on arm64 deferred"

The reporter ran the map example's `--probe` live on Apple Silicon. Also filed under "not
reproducible": `UI.md` claims `new CanvasView()` with a field-assigned `onPaint` crashes on the
Cocoa host; it ran fine on v0.11.0. Re-verify both claims on the current tree and update or delete
them.

## 4. `doc/LANGUAGE.md` reserved-keyword list is incomplete

See [[import-clause-words-globally-reserved]] (p2) - `from`, `lib`, `cache`, `define`, `package`,
`program`, `framework`, `pri` are all reserved in practice and none are listed. If that issue is
fixed by making them soft keywords the list needs no change; if not, the list must be corrected.

## 5. Nothing warns that an owning local + a borrowing `list<T*>` is a use-after-free

Added 2026-08-21 from the same reporter (their issue 16). "A local variable owns the pointer it
allocates" and "`list<T*>` borrows its elements" are both documented and both correct; composing
them in a loop silently yields a container of dangling pointers with no diagnostic. Until the
analysis in [[delete-borrow-via-named-local]] exists, the container docs must state the rule
outright and lead with `list<unique Node*>` + `add(move n)`. See that file for the measured repro.

## 6. The `manifest` declaration is documented nowhere

Added 2026-08-21 (MemPressMonitor Win32 port). Filed separately as
[[manifest-declaration-is-undocumented-and-unvalidated]] because it has a code half (validation /
`--dump-manifest`) as well as a docs half.
