# Static analysis: clang-tidy first, clang-format later

Status: Steps 1-2 DONE 2026-11-07 on Windows (see "Step 1 results", "Step 2 baseline"); Step 3 is the maintainer's.
PROPOSED 2026-11-06. Rulings 2026-11-06: D2, D3, D5 RULED; D1 settled
(maintainer provides the clang-tidy executable); D4, D6 and D5 enforcement DEFERRED - maintainer
integrates them later; D7 open. Steps 1-2 can start.

clang-format is DEFERRED until clang-tidy lands (maintainer, 2026-11-06): it is layout only, finds no
bugs. Its notes are kept at the end so the later step does not start from scratch.

## Goal

Catch real defects in cflat's own C++ (use-after-move, dangling handles, null paths, needless copies)
in code written by agents, at review time, without a big-bang cleanup of the existing 12 MB.

## Current state (verified 2026-11-06)

- No `.clang-tidy`, `.clang-format`, or CI. Only style config: VS-generated `.editorconfig`.
- No `compile_commands.json`: `CMAKE_EXPORT_COMPILE_COMMANDS` is not set in `CMakePresets.json`.
  Ninja supports it. Build dir is `build/<preset>/` (in-tree, gitignored).
- 224 `.cpp` files, 12.4 MB. Largest: `MainListener_PostfixExpression.cpp` 1.2 MB,
  `MainListener_Declarations.cpp` 1.0 MB, `LLVMBackend.h` 0.9 MB (included by most TUs).
- MSVC flags: `/permissive- /MP /Zc:preprocessor`, `/bigobj` on big TUs, PCH via
  `target_precompile_headers(cflat PRIVATE cflat/pch.h)`. No explicit `/W` level.
- ANTLR output is generated into `build/<preset>/antlr_generated/` - out of `cflat/`.
- clang-tidy binaries available:
  - VS 18 bundle: `VC\Tools\Llvm\x64\bin\clang-tidy.exe`, version 22.1.3. No `clang-tidy-diff.py`,
    `run-clang-tidy.py`, or `clang-apply-replacements.exe` ship with it.
  - Pinned LLVM install (23.1.0) has NO clang-tidy: `bootstrap.bat` builds
    `LLVM_ENABLE_PROJECTS="clang;lld"` only.
  - Scripts ARE in the pinned source clone:
    `%USERPROFILE%\.cflat-compiler-deps\src\llvm-project\clang-tools-extra\clang-tidy\tool\`
    (`clang-tidy-diff.py`, `run-clang-tidy.py`). Python 3.12 is on PATH.
- clangd 22.1.0 installed (winget). It reads `.clang-tidy` and shows diagnostics in the editor.

## Decisions (need ruling)

### D1. Which clang-tidy binary

| Option | Cost | Risk |
|---|---|---|
| a. VS-bundled 22.1 | none, already installed | one major behind pinned LLVM 23 headers; parse should work, check output may differ from 23 |
| b. add `clang-tools-extra` to `bootstrap.bat` LLVM build | rebuild of the release LLVM tree (build tree is deleted after install); `ninja clang-tidy clang-apply-replacements` only | slow once; exact version match, provisioned for every clone |
| c. official LLVM 23.1 Windows release next to the deps tree | one download | a second LLVM install to keep in sync; version pin must be checked by the script |

SETTLED 2026-11-06: the maintainer provides the executable; this plan assumes `clang-tidy` is
present and does not provision it. Scripts come from the pinned source clone (Python is already a
dependency).

### D2. Check set - RULED 2026-11-06 (accepted as written)

Start with checks that find bugs, not style. `WarningsAsErrors` empty at first (see D4).

On:
- `bugprone-*` minus `bugprone-easily-swappable-parameters`, `bugprone-narrowing-conversions`
  (very noisy on LLVM `unsigned`/`size_t` APIs; revisit after the baseline)
- `clang-analyzer-core.*`, `clang-analyzer-cplusplus.*`, `clang-analyzer-deadcode.*` - DROPPED 2026-11-07
  for cost, with `bugprone-unchecked-optional-access` (see Step 1 results)
- `performance-*` (`unnecessary-copy-initialization`, `unnecessary-value-param`, `for-range-copy`,
  `move-const-arg`, `inefficient-string-concatenation`)
- `misc-redundant-expression`, `misc-unused-using-decls`
- `modernize-use-override`, `modernize-use-nullptr`
- `readability-misleading-indentation`, `readability-redundant-control-flow`

Report-only, never gated: `readability-identifier-naming`, `readability-identifier-length` (D5).

Off (churn, not bugs, or broken by this codebase's shape):
- `readability-function-cognitive-complexity`, `readability-function-size` - every listener function
- `misc-include-cleaner` - fights the PCH
- `readability-magic-numbers`, `cppcoreguidelines-*`, `modernize-use-trailing-return-type`
- `google-*`, `fuchsia-*`, `hicpp-*`, `altera-*`, `llvm-*`, `llvmlibc-*`

Scope: `HeaderFilterRegex` matches cflat's own headers under `cflat/` only - not LLVM, Clang, ANTLR
runtime, vcpkg, or `antlr_generated/`. `ExcludeHeaderFilterRegex` for `antlr_generated`.

### D3. What gets checked - RULED 2026-11-06: (a) changed lines + one baseline run

| Option | Notes |
|---|---|
| a. changed lines only (`git diff master` -> `clang-tidy-diff.py`) | fast; old code untouched until edited |
| b. changed files, whole file | a 1 MB TU dumps its whole backlog on whoever touches it |
| c. whole tree | minutes per big TU with LLVM headers; weak box, 2-worker cap |

Recommendation: (a) for the gate. Plus ONE whole-tree baseline run (Step 2) in the background to size
the backlog per check; never as a gate.

### D4. Enforcement - DEFERRED 2026-11-06 (maintainer integrates later)

Options below kept as input for that integration; nothing here is built by this plan.

| Option | Notes |
|---|---|
| a. report only | easy to ignore |
| b. gate on a short high-confidence list | `WarningsAsErrors` for e.g. `bugprone-use-after-move`, `bugprone-dangling-handle`, `clang-analyzer-core.NullDereference`; rest reported |
| c. gate on everything enabled | false positives block merges |

Recommendation: (a) until the diff runner exists (Step 3), then (b) once the baseline shows the gated checks at zero (fix the
existing hits first, as their own change).

### D5. Naming - RULED 2026-11-06 (maintainer, C#-style)

Convention for new and edited declarations:

| Kind | Style | Example |
|---|---|---|
| class / struct / enum / typedef / alias | PascalCase | `TypeAndValue` |
| free function, public method | PascalCase | `GetOrParseFile` |
| private / protected method | camelCase | `resolveOverload` |
| public data member | PascalCase | `IsOwning` |
| private / protected data member | camelCase, no prefix or suffix | `parseTreeCache` |
| local variable, parameter | camelCase | `typeName` |
| constant (constexpr, static const, global const) | PascalCase, no `k` | `ParseTreeMaxAge` |

Rule of thumb: lowercase first letter = private or local, capital = public or a constant.

Name length: at least 3 characters. Only exception: `i`, `j`, `k` anywhere (loop counters, array
indices, in or outside a `for` header). No `it`, `e`, `a`/`b`: use `itr`, `exc`/`err`, `lhs`/`rhs`.

The lowercase-private rule covers methods as well as data members (ruled 2026-11-06). Free
functions are PascalCase whatever their linkage (`static` in a `.cpp` included).

Not ruled (left unchecked): enum constants, non-const globals, namespaces, macros, template
parameters.

Existing code that differs (measured 2026-11-06 in `cflat/*.h`, rough regex count): ~670 private
members with a trailing `_` (`parseTreeCache_`), 66 `k` constants (`kParseTreeMaxAge`). Measured
public fields are already PascalCase (~720). Existing private-method names were not measured;
any PascalCase ones now differ too.

Enforcement (DEFERRED 2026-11-06 - maintainer integrates later; the convention above stands):
- `readability-identifier-naming` + `readability-identifier-length`, changed lines only, REPORT
  ONLY - never in the D4 gated list. The checks flag a declaration, so an edit to an old
  `foo_` declaration line reports it; renaming an existing name is its own change (cross-TU rename
  via exported fixes + `clang-apply-replacements`), never folded into an unrelated fix.
- Overrides of external bases (ANTLR `enterX`/`exitX`, Clang `ASTConsumer`, LLVM passes) are
  expected to be skipped by the check because the base owns the name; Step 2 confirms, add
  `MethodIgnoredRegexp: '^(enter|exit|visit)[A-Z].*'` only if it does not.

Config sketch (Step 2 verifies each key against real hits):

```yaml
CheckOptions:
  readability-identifier-naming.ClassCase: CamelCase
  readability-identifier-naming.StructCase: CamelCase
  readability-identifier-naming.EnumCase: CamelCase
  readability-identifier-naming.UnionCase: CamelCase
  readability-identifier-naming.TypedefCase: CamelCase
  readability-identifier-naming.TypeAliasCase: CamelCase
  readability-identifier-naming.FunctionCase: CamelCase
  readability-identifier-naming.PublicMethodCase: CamelCase
  readability-identifier-naming.PrivateMethodCase: camelBack
  readability-identifier-naming.ProtectedMethodCase: camelBack
  readability-identifier-naming.PublicMemberCase: CamelCase
  readability-identifier-naming.PrivateMemberCase: camelBack
  readability-identifier-naming.ProtectedMemberCase: camelBack
  readability-identifier-naming.LocalVariableCase: camelBack
  readability-identifier-naming.LocalConstantCase: camelBack
  readability-identifier-naming.ParameterCase: camelBack
  readability-identifier-naming.ConstantParameterCase: camelBack
  readability-identifier-naming.ConstexprVariableCase: CamelCase
  readability-identifier-naming.GlobalConstantCase: CamelCase
  readability-identifier-naming.StaticConstantCase: CamelCase
  readability-identifier-naming.ClassConstantCase: CamelCase
  readability-identifier-length.MinimumVariableNameLength: 3
  readability-identifier-length.MinimumParameterNameLength: 3
  readability-identifier-length.MinimumLoopCounterNameLength: 3
  readability-identifier-length.MinimumExceptionNameLength: 3
  readability-identifier-length.IgnoredVariableNames: '^[ijk]$'
  readability-identifier-length.IgnoredParameterNames: '^[ijk]$'
  readability-identifier-length.IgnoredLoopCounterNames: '^[ijk]$'
  readability-identifier-length.IgnoredExceptionVariableNames: ''
```

Do NOT set the generic `ConstantCase` or `ConstantMemberCase`: clang-tidy applies them to every
const-qualified local, parameter and field first, which would demand PascalCase for
`const auto& name = ...`. Only the specific constant keys above are set.

### D6. Where it runs - DEFERRED 2026-11-06 (maintainer integrates later)

Options below kept as input; the diff-gate design notes apply to whatever runner is built.

- `lint.bat` at repo root (Windows), next to `test.bat`: diff vs `master`, `-j 2`, exits non-zero
  on gated findings. `lint.sh` counterpart for macOS (see "macOS").
- One step in `internal/skill/fix-issue/SKILL.md`: run `lint.bat` before the review agent; review
  reads its output. With no CI, this is the real enforcement point.
- No git pre-commit hook (agents work in worktrees and the user commits; a hook adds friction for
  little gain).

### D7. Other checks - in this plan or separate (OPEN)

| Item | Recommendation |
|---|---|
| repo-rules script (`utilities/lint_rules.py`): non-ASCII in source/docs, `std::cout` / `LogWarning` in `cflat/`, hand edits to `cflat/locales/` | in this plan (Step 3): cheap, enforces CLAUDE.md rules clang-tidy cannot see |
| MSVC `/W4` on cflat's own sources | separate, later: first pass needs fixes across the tree |
| `ruff` (9 Python files), `shellcheck` (`test.sh`, `cmake_build.sh`) | optional, any time |

## Diff-gate design notes (both hosts)

- **Changed headers.** `clang-tidy-diff.py` runs clang-tidy on each changed file, headers included.
  A header has no compile-database entry, so a header-only diff (e.g. `LLVMBackend.h`) gets a
  flagless or empty run. Fix in the wrapper: for each changed header, pick one TU in the database
  that includes it (one small TU covers `LLVMBackend.h`, included almost everywhere) and run that
  TU with a `-line-filter` naming the header's changed ranges. `HeaderFilterRegex` must admit the
  header or the filter reports nothing.
- **Files not in the database.** Drop diff entries for files absent from `compile_commands.json`
  (other host's sources, docs, `.cb`), else clang-tidy runs them with no flags and reports bogus
  parse errors.
- **PCH flags.** Both hosts put precompiled-header flags in the database (MSVC `/Yu` `/Fp` `/FI`;
  clang `-Xclang -include-pch`). A `.pch` from a different compiler version is refused. Strip them
  in the wrapper; Step 1 confirms the exact flags.
- **Version in output.** Wrapper prints `clang-tidy --version` first, so a finding is traceable to
  the binary that produced it.

## macOS (from memory, not yet verified on a Mac)

- Assumes a clang-tidy executable is present (decision D1 covers where it comes from on Windows;
  on macOS Homebrew `llvm` ships it, keg-only, so call by full path; it tracks the latest release,
  not pinned 23.1).
- Same `.clang-tidy` file. Path regexes use `[\\/]` so one config serves both separators.
- Scripts: run under `python3`, from the pinned source clone or Homebrew `llvm` (`bin/run-clang-tidy`,
  `share/clang/clang-tidy-diff.py`).
- `lint.sh` mirrors `lint.bat`, including the design notes above:
  `git diff -U0 master | python3 clang-tidy-diff.py -p1 -path build/<mac-preset> -j 2 -clang-tidy-binary "$CLANG_TIDY"`.
- Easier than Windows: the database holds clang flags, read natively (no MSVC driver translation).
- Each host lints only its own TUs: `Winmd*.cpp` is Windows-only, `WinmdStubs.cpp` is non-Windows.
  Matches the verify-on-your-host rule; no cross-host lint.

## Step 1 results (2026-11-07, Windows, VS-bundled clang-tidy 22.1.3)

- `CMAKE_EXPORT_COMPILE_COMMANDS: ON` in the `vcpkg-base` preset (covers win-x64 and macos-arm64).
  `cmake_build.bat release` writes `build/win-x64-release/compile_commands.json`, 61 entries.
- PCH: the database carries `/Yu<cmake_pch.hxx> /Fp<...>.pch /FI<cmake_pch.hxx>`; clang-tidy fails
  with "input is not a PCH file". `utilities/tidy_ccdb.py BUILD_DIR` writes
  `BUILD_DIR/tidy/compile_commands.json` without `/Yu` `/Fp` (and clang `-include-pch`), keeping
  `/FI` so pch.h is still included. Use `-p BUILD_DIR/tidy`. No parse errors on the two probes.
- Timing (single run each, Windows box shared, so rough):
  - `JsonRpcLoop.cpp`: 12 s, 2 findings (`performance-unnecessary-value-param`).
  - `MainListener_PostfixExpression.cpp` (1.36 MB) with the full D2 set: killed after 40 min (one
    core, 3.7 GB). Parse + one trivial check: 8 s. Without `clang-analyzer-*` and
    `bugprone-unchecked-optional-access`: 20 s, 143 findings. `bugprone-use-after-move` alone: 11 s.
    The other flow checks (`infinite-loop`, `redundant-branch-condition`,
    `unnecessary-copy-initialization`, `unnecessary-value-param`) together: 15 s. So the cost is the
    path-sensitive analyzer plus the optional-access dataflow check (by elimination), both now off.
- Big-TU findings by check (before the optional-access drop, analyzer already off): enum-size 49,
  inefficient-string-concatenation 31, exception-escape 16, inefficient-vector-operation 8,
  misleading-indentation 8, branch-clone 7, throwing-static-initialization 6, a few singles.
  `performance-enum-size` and `bugprone-exception-escape` look like noise candidates for Step 2.
- macOS: no clang-tidy on the Mac yet (D1), nothing probed there.
- RULED 2026-11-07 (maintainer): a lint that takes 40 min on one file is not useful, however
  correct. Every check in `.clang-tidy` must keep the largest TU at well under a minute. Do not
  re-enable `clang-analyzer-*` or `bugprone-unchecked-optional-access` in the shared config; if
  wanted later, run them as a separate opt-in pass over changed functions only, with a per-file
  timeout.

## Step 2 baseline (2026-11-07, Windows, master 80aae14f)

`run-clang-tidy.py -j 2 -p build/win-x64-release/tidy "cflat[\\/][^\\/]+\.cpp$"`: 61 TUs in 4 min 25 s,
852 unique findings, 1 parse error (`RoMetadataApi.h`: 'cor.h' not found - run from a vcvars64
shell so the NETFX SDK include dir is on INCLUDE; the ssh shell had none). Findings by check:

| Check | Count | Outcome |
|---|---|---|
| performance-inefficient-string-concatenation | 232 | p3/clang-tidy-performance-backlog |
| bugprone-derived-method-shadowing-base-method | 91 | turned OFF: every RecursiveASTVisitor `Visit*` (CRTP by design) |
| performance-enum-size | 72 | turned OFF: int-backed enums, no defect |
| bugprone-exception-escape | 68 | p3/clang-tidy-exception-escape |
| bugprone-branch-clone | 64 | kept, report only; mostly intentional parallel arms |
| bugprone-unused-return-value | 58 | p3/clang-tidy-ignored-return-values |
| performance-unnecessary-value-param, inefficient-vector-operation, no-automatic-move, unnecessary-copy-initialization, for-range-copy | 44, 43, 39, 24, 4 | p3/clang-tidy-performance-backlog (one for-range-copy fixed) |
| readability-misleading-indentation | 24 | 4 fixed (one real: an unbraced `else` in MainListener_Aggregates.cpp ran the cast on the struct arm too); 20 are long `else if` chains whose head `if` sits at another column - cosmetic, left |
| bugprone-nondeterministic-pointer-iteration-order | 7 | p3/clang-tidy-nondeterministic-pointer-iteration |
| bugprone-use-after-move | 2 | both fixed; one real: a same-basename interface collision logged its error twice, the second time empty |
| misc-redundant-expression | 2 | fixed (`? 1 : 1`, identical atomic op arms) |
| bugprone-unused-local-non-trivial-variable | 3 | fixed (removed) |
| modernize-use-override | 1 | fixed |
| bugprone-sizeof-expression | 2 | false positive: pointer bytes appended to a key on purpose |
| bugprone-redundant-branch-condition | 2 | left: harmless re-test inside a guarded block |
| readability-redundant-control-flow | 3 | left: the trailing `continue` / `return` documents intent |
| switch-missing-default 4, inc-dec-in-conditions 4, unchecked-string-to-number (atoi) 4, empty-catch 14, assignment-in-if 11, throwing-static-init 15, no-int-to-ptr 9, others 1-2 | | left, report only |

## Steps

- **Step 1 - compile database + parse probe.** `CMAKE_EXPORT_COMPILE_COMMANDS: ON` in the `win-x64` base
  preset. Run clang-tidy on one small TU and one big TU (`MainListener_PostfixExpression.cpp`);
  record wall time here. Known risk: MSVC PCH flags (`/Yu`, `/Fp`, `/FI cmake_pch.hxx`) in the
  database. Strip via `--extra-arg`/`--extra-arg-before` or a wrapper if the parse fails; same for
  `.clangd` `CompileFlags: Remove`. Probe output to `scratch/`.
- **Step 2 - `.clang-tidy` + baseline.** Commit the D2 config. One whole-tree run, background,
  `run-clang-tidy.py -j 2`, findings to `scratch/tidy_baseline/`. Record count per check here. Drop
  or tune any check with a high false-positive rate before it reaches the gate.
- **Step 3 - integration: MAINTAINER, later (D4, D6 deferred).** Diff runner (`lint.bat` /
  `lint.sh` or other), `fix-issue` step, gating. Inputs: D4/D6 options and the diff-gate design
  notes (header routing, database filter, PCH strip, version line). `utilities/lint_rules.py`
  waits on D7.
- **Step 4 - later.** Optional renames of existing names to the D5 convention (one kind per change),
  `/W4`, clang-format (below).

## Acceptance

- `compile_commands.json` produced by `cmake_build.bat release` with no other build change.
- clang-tidy parses every cflat TU in the database (baseline run has no fatal parse errors).
- For the diff runner, when integrated: a clean `master` diff exits 0; a planted `use-after-move`
  is reported; a planted finding in a header-only diff (`LLVMBackend.h` inline function) is
  reported; a diff touching only a file outside the database (e.g. a `.md`) runs no clang-tidy.
- `.\test.bat Release` unchanged (no compiler source change in Steps 1-3).
- Baseline counts and big-TU timing recorded in this file.

## Deferred: clang-format notes

- Use pinned `clang-format.exe` 23.1.0 from `%USERPROFILE%\.cflat-compiler-deps\llvm-23.1.0\bin\`;
  script checks `--version`.
- Match existing style (sampled `LLVMBackend_Overloads.cpp`: ~90% own-line braces, 4 spaces, no
  tabs, `T* x`, 21/11991 lines over 120): `BasedOnStyle: Microsoft`, `PointerAlignment: Left`,
  `NamespaceIndentation: All`, `IndentCaseLabels: false`, `SortIncludes: Never`,
  `ReflowComments: false`.
- Changed lines only (`git clang-format master`), check-only gate. No whole-tree reformat while
  worktrees are in flight; if ever done, pair with `.git-blame-ignore-revs`.
