# `-g` on macOS produces an executable with no reachable debug info

Compiling with `-g` succeeds and the linked Mach-O carries a debug map, but the object
file the map points at is deleted right after the link and no `.dSYM` is produced. The
result is an executable a debugger cannot get a single source line out of.

## Repro

```
x64/Release/cflat scratch/dogfood/lib/tooling.cb -g -o scratch/dogfood/lib/tooling -i Test/library
dwarfdump --debug-info scratch/dogfood/lib/tooling
```

## Observed

```
scratch/dogfood/lib/tooling:	file format Mach-O arm64
.debug_info contents:
                                  <- empty; --debug-line is empty too
```

```
nm -pa scratch/dogfood/lib/tooling | grep OSO
0000000000000000 - 00 0001   OSO /Users/.../scratch/dogfood/lib/tooling.o

ls scratch/dogfood/lib/tooling.o
ls: No such file or directory

dsymutil scratch/dogfood/lib/tooling
warning: (arm64) .../tooling.o unable to open object file: No such file or directory
warning: no debug symbols in executable (-arch arm64)
```

The program is a deliberate null deref inside `sumChain`; it faults with 139 as intended,
but a debugger has nothing to map the faulting PC to.

## Expected

`-g` should leave the debug info reachable: either run `dsymutil` as the last link step to
produce `<out>.dSYM`, or keep the `.o` next to the output. A `-g` build whose only visible
effect is +1.8 KB of dangling debug map is worse than no `-g`.

## Note on lldb in this environment

`lldb -b -o run -o bt <exe>` hangs at `run` here for a plain `clang -g` C binary too, so
the hang is the sandbox refusing debugserver, NOT a cflat defect. The finding above is
purely static (dwarfdump / dsymutil) and does not depend on running a debugger.

## Fix direction

Find where the driver unlinks the intermediate object after invoking `ld64.lld` (the
"Linking (ld64.lld, SDK-free)" path). When `-g` is set, skip the unlink and additionally
shell out to `dsymutil` if present, emitting `<output>.dSYM`. Mirrors what clang's driver
does on Darwin. Windows (PDB via lld-link) is unaffected.
