# Test-leg authoring rules are enforced only by reviewers

Filed 2026-11-06 as tech debt (maintainer: p2). Five authoring rules in `internal/fix-issue-lessons.md`
("On tests", "Refinements") are checked by a reader, never by `test.sh` / `test.bat`. Each one has
shipped a dead or platform-red leg at least once. Mechanize them as a lint pass the suites run.
Retire the matching lessons bullet in the same change.

## Checks

1. **Bare-leg count (fail).** A bare `expect_error("...");` leg ends the check, so legs after it in
   compile order never run, but the file still PASSES (2026-09-10: six dead legs in three err files).
   For each `Test/errors/**/err_*.cb`, the number of `PASS: expected error received` lines must equal
   the number of `expect_error` legs the file arms. `test_err.bat` / `run_err` already capture the
   output. The counting rule has to handle legs inside `if const` arms that are not taken on this
   host, and sidecar `expect-exit` files.
2. **Substring source (fail).** `expect_error` matches the `LogError*` FORMAT STRING, not the
   printed `en.json` text (T110). Every substring must occur in a format-string fragment in `cflat/`
   source. `utilities/extract_diagnostics.py` already extracts them; split each template at its
   `{N}` placeholders and accept a substring that falls inside one literal run or spans placeholders.
3. **Platform-bound pins (fail).** No path separator, no drive letter, and no mangled generic name
   (`$`-joined, e.g. `std.map$cplv.Key$int.insert`) in a substring. A basename plus `(line,col)` tail
   is allowed (`err_duplicate_interface_name.cb:41`). The `$` pins exist today in
   `err_cpp_member_signature_contains_errors.cb` and `err_cpp_set_map_key_lvalue.cb`, and they break
   whenever `MangleTypeArg` changes. Prefix-pin them, or pin the source spelling, until
   `mangled-generic-name-leaks-into-diagnostics` lands.
4. **Tautological legs (fail).** `Test("name", <lit>, <lit>)` with equal literals can never fail.
   Exception: a leg whose only job is to prove an `if const` arm was taken
   (`test_basic.cb` `if_const_*`, `hex_ladder_if_const_true`). Exempt a literal leg that is the
   direct body of an `if const` arm, and flag everything else.
5. **LLP64 width (report).** A bare `long` (not `long long`) or a non-fixed enum width asserted in a
   `Test/*.cb` leg outside `if const (!__WINDOWS__)` is Windows-red (fix box 2026-10-15 shipped
   three such legs). This check is heuristic: print the hit list, do not fail.

## Acceptance

- One lint entry point, run by both `test.sh` and `test.bat` before the workers launch. Failing
  checks count as failed tests, with file:line. Check 5 prints and does not count.
- Each failing check is mutation-proven: plant a violating leg in a scratch copy and the lint goes
  red; remove it and it goes green.
- Existing violations are fixed in the same change (checks 3 and 4 have live hits today). Nothing is
  grandfathered in an allowlist.
- In `fix-issue-lessons.md`, the bullets for checks 1-4 shrink to one line pointing at the lint.
