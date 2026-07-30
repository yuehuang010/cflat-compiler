# Assigning a stack address to a 'unique T*' field aborts with no diagnostic

Filed 2026-07-30, found in passing by the round-2 review of
[[interface-issue-queue]] (landed design records). Pre-existing, identical on both binaries, and
unrelated to the generic key space - the generic wrapper is incidental.

Severity: **silent abort (exit 134), no diagnostic at all.** Per CLAUDE.md's debugging convention,
an abort reachable from plain source must become a proper compile-time error once diagnosed.

## Repro

`scratch/rev6/p4f_unique_stackaddr.cb` - a `unique T*` field given the address of a stack local:

```cflat
struct Box<T> { T t = default; };
// ... Box<unique Item*> b = default; b.t = &i;   where i is a stack local
```

```
Abort trap: 6
```

Verified exit code **134** on both `x64/Release/cflat` and the pre-change binary. No message is
printed on either - not a `LogError`, not an LLVM assert text, nothing.

## Why it should be an error, not an abort

A `unique` pointer owns its pointee and frees it. A stack address is not ownable: the eventual
release is undefined behaviour on memory the callee never allocated. So the correct outcome is a
compile-time rejection at the assignment, in the same family as the existing ownership diagnostics.

## Root cause direction

Not investigated. The silence is the notable part - an abort with no output suggests the failure is
in the destructor/release path being synthesized for the field rather than in a checked assignment
path, so no diagnostic site is reached. Compare against the equivalent assignment to a plain
`unique T*` LOCAL, which may already be diagnosed; if it is, the field path is simply missing the
check the local path has.

## Test coverage

None. Once diagnosed, this wants a `Test/errors/err_*.cb` leg pinning the message substring.

Related: [[interface-issue-queue]]
