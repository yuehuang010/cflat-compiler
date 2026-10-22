# A bare expect_error inside a scoped expect_error block swallows the outer block's verdict

Severity p3 (test infrastructure: a missing expected error passes silently). Left by T354 (2026-10-20), which made scoped/scoped,
bare-outer/scoped-inner and file-scope/function-bare nesting a proper stack; master behaves the same.

Repros: scratch/repro_keep/box1021/t354_rev/cases/p20_scoped_outer_bare_inner.cb, p21_scoped_outer_bare_inner_nested_block.cb.
Outer scoped block expects qA (never raised); an inner BARE `expect_error("qB");` consumes qB -> rc 0 PASS (should be rc 1,
"expected error ... did not occur" for qA).

Root cause (T354 review): the bare form is only caught at function level, so the inner error reaches the scoped statement catch
(MainListener_Statements.cpp ~5332), which sets errorReceived for the OUTER scope.
Fix direction: in that catch record whether `activeExpectScope_ == expectScope` before RestoreExpectScope (~5385); if an inner scope
consumed the error, restore first and then run the outer's "did not occur" check (~5389-5399). Audit the file-scope catch
(MainListener_Declarations.cpp ~3640/3674) and ForwardRefScanner.cpp ~2721 the same way.
