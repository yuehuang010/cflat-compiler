# Unwind cleanup chain re-emits every live local per invoke before deduping

Found 2026-10-13 (Windows perf box). Pad IR is shared now on both EH models (Itanium landing
pads and Windows funclets: each local's cleanup is its own `cleanuppad`, `cleanupret unwind
label` the next), but ChainUnwindCleanup still EMITS each unit into a fresh block and only then
hashes / compares / erases it. IR-building work stays O(invokes x live locals) even though the
final IR is O(locals).

Measured (Windows Release, Test/test_cpp_interop.cb, cdb stack sampling of `--check`):
ChainUnwindCleanup on ~35-40% of samples (DropValue -> EmitOwningPtrCleanup /
GetOrCreateFullDestructor / GetType lookups, plus UnwindRegionHash / SameUnwindRegion).
`--check` 29 s, CLI compile 29.5 s, LSP single-file 28.7 s; this fixture is the LSP bulk
sweep's critical path (pool 8: sweep wall == this file, 35 s). After the region index moved to
one SmallDenseMap built once per link: `--check` 27 s, hash+compare ~2 of 7 chain samples; the
rest is emission.

Fix direction: skip emission when a cached region for the same unit + continuation is provably
what DropValue would emit. Key must cover every input of DropValue and its callees: the
NamedVariable fields read (IsMoved, ExplicitlyMovedNull, ConditionalDropFlag, OwnFlag/
OwnFlagActive, RawArrayLength*, AllocAlignment, Storage/BaseType, ...), backend state
(returnedStructDtorShellAlloca / SkipAlloca), type-level answers that change as types complete
(GetFullDestructorForDelete deferred stubs), and side effects (NoteOwnSlotLeavingLoad,
first-time GetOrCreateFullDestructor). Not attempted: needs that audit first.

Secondary lever (maintainer, 2026-10-13): LSP/--check never emit a binary, so pad emission
could be skipped there. Open question before doing it: can any diagnostic come only from pad
emission (a DropValue LogError not also reached on the normal scope-exit path)? If yes the
editor silently loses it.
Measured 2026-10-13 (temp env-gated skip in CreateCallOrInvoke when symbolSink_ set, since
removed): LSP bulk sweep 237 files, diagnostics byte-identical (1030 lines), wall 47.2 -> 45.0 s
(pool 4 is capacity-bound, not bound by this file). Where it pays is editor latency on
test_cpp_interop.cb: open 28 -> 20 s, each didChange re-analysis 25 -> 17 s (clang group is
reused on edits; samples put ~60% of an edit in ChainUnwindCleanup, mostly DropValue emission).
Same numbers bound the emission memo above, which would also help the CLI.
