# The same binary emits different IR run-to-run (switch-case ordering)

Filed 2026-07-29. PRE-EXISTING and compiler-wide, not tied to any feature.

Severity: no known miscompile - both variants are semantically equivalent. The damage is to
METHODOLOGY: "I diffed the emitted `.ll` and got zero differences" is the standard proof in
this repo that a refactor is behaviour-preserving, and that proof is luck-dependent while
this exists. A spurious diff can also send a future investigation chasing a phantom.

## Repro

```bash
for i in $(seq 1 8); do
  x64/Release/cflat Test/test_c.cb -i Test/library --no-cache -l scratch/td_$i.ll >/dev/null 2>&1
done
md5 -q scratch/td_*.ll | sort -u | wc -l     # prints 2 or 3, not 1
```

There are at least THREE variants, not two. An 8-run sample happens to hit two of them; a
20-run sample of each of two binaries produced the same set of three (13/5/2 and 14/5/1). So
a small repeat-count check can easily miss a 2-in-20 variant - size the sample accordingly.

Observed 8 runs of one binary splitting into two hashes (5 of one, 3 of the other). The whole
difference is the ORDER of two cases in one switch:

```
1926d1925
<     i32 2, label %switchCase2
1927a1927
>     i32 2, label %switchCase2
```

Reproduces with and without the core bitcode cache, so it is not a cache artifact.

## Root cause

Not investigated. The shape - a stable set whose ORDER varies between runs of the same
binary - is the signature of iterating a container keyed on pointers or on an unordered hash
whose seed/addresses vary per process (`std::unordered_map`/`set` of pointer keys, or ordering
by an address). Find the switch-case collection in the switch codegen path and check what it
iterates.

## Fix direction

Make the iteration order deterministic (sort by case value, or key the container on something
stable). Until then, anyone using IR diffs as proof should run the emit at least twice per
side and treat a diff that is not stable across repeats as noise - and say which files were
covered, since not every module is affected (`Test/test_c.cb` is; the twelve modules used for
the closure-lowering proof were stable across repeated runs).
