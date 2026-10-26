# expect_error fails to match emitted type diagnostic

Summary: `expect_error` does not match a type lookup error even when the requested substring is present.
Repro: `expect_error("cannot find the type")` around a failing type lookup.
Master vs expected: master fails to match its emitted diagnostic; expected the substring match to pass.
Found: T463 r1 sol review, 2026-10-24 box, PRE-EXISTING
