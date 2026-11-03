# expect_error fails to match emitted type diagnostic

Summary: `expect_error` does not match a type lookup error even when the requested substring is present.
Repro: `expect_error("cannot find the type")` around a failing type lookup.
Master vs expected: master fails to match its emitted diagnostic; expected the substring match to pass.
Found: T463 r1 sol review, 2026-10-24 box, PRE-EXISTING
Root cause (NATFIX, 2026-10-30): `expect_error` matches the source-English `LogError` format text (`unknown type '{}'`), never
the localized catalog text ("cannot find the type" exists only in en.json). Widening the matcher to printed text breaks the
test.sh pseudo-locale discovery pass (~line 370), whose comment says source-English matching is by design.
MAINTAINER QUESTION: (a) keep source-English matching and close this as usage (recommended), or (b) match printed text and
rework the discovery pass with it.
