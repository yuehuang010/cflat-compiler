# `--symbol-dump-ir` / `--symbol-dump-opt` ignore `--sanitize=ownership`

Filed 2026-09-03 while measuring the `init_capacity` poison fill.

## Summary

Module dumps with and without `--sanitize=ownership` are byte-identical, while the compiled
program clearly differs (the sanitizer-gated `if const (__SANITIZE_OWNERSHIP__)` fill appears in
the `-l` IR and runs). The dump path builds its own compile options and does not carry the
sanitizer flag, so the gated IR cannot be inspected through the symbol-dump family.

## Repro

    cflat probe.cb --sanitize=ownership --symbol-dump-ir function:main   # same as without the flag
    cflat probe.cb --sanitize=ownership -l out.ll                          # differs (memset present)

## Fix direction

Thread the sanitizer flags (asan, ownership) from ArgParser into the options the symbol-view
compile uses, the same way `-O` levels already reach `--symbol-dump-opt`. Add a check in the
existing symbol-dump coverage if there is one for flags (grep Test/ and vscode-extension/test/
for `symbol-dump`).
