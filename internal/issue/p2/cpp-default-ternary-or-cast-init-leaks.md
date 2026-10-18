# Default ternary or cast initialization leaks C++ owners

Pre-existing on master. Measured 2026-10-16 with
`bash scratch/rev_t306_opus_probe.sh`; the `M` column used
`/Users/felixhuang/source/cflat-compiler/x64/Release/cflat` without rebuilding it.

## Repros and measurements

Counter order is ctor/copy/move/copy-assign/move-assign/dtor/live.

| CFlat repro | Master | clang++ C++20 oracle |
|---|---:|---:|
| `Twin x = c ? default : rettw();`, c=true (`F_decl`) | `1100011` | `1000010` |
| `return c ? default : rettw();`, c=true (`G_return`) | `1100011` | `1000010` |
| `Twin x = (Twin)default;` (`AA_declcast`) | `1100011` | `1000010` |
| `x = (Twin)default;` (`AB_assigncast`) | `2001011` | `2000120` |
| CFlat `ctake(c ? default : move tw)`, c=false/true (`C2_cflatfn_move`) | `1100020` / `2100021` | `1010020` / `2000020` |

The ternary/cast default value survives as a second or undestroyed temporary in declarations,
returns, assignments, and CFlat by-value calls. Round 1 fixed the C++ call-argument slot path;
these sinks remain unchanged on this branch. Confirm each path from `--no-opt` IR before
choosing a shared or per-sink fix.

## Fix direction

Follow the default-created C++ temporary through each sink and preserve the selected prvalue's
ownership slot. Add value, copy/move, destructor, live-count, and leak checks for every sink.
If a sink cannot be lowered safely, reject it with a diagnostic true of that exact construct.
