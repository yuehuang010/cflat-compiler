# MSVC frame of ParsePostfixExpressionInner is 965 KB, so Windows nesting depth is ~9x shallower than Mac

Summary: Under MSVC, MainListener::ParsePostfixExpressionInner (MainListener_PostfixExpression.cpp, ~11.8k lines) allocates
965,320 B of stack per call; MSVC does not share slots between temporaries of disjoint scopes. Other big frames: its lambda_30
185 KB, ParseAssignmentExpression 170 KB, EmitReturnExpression 130 KB, ParseDeclaration 121 KB (dumpbin /unwindinfo).
Effect: `((x)+1)` nesting costs 2.33 MB per level on Windows vs 249 KB on Mac; a nested pointer ternary ~1.3 MB vs ~210 KB.
On the 64 MB Windows main stack that is ~28 plus-levels; on a 16 MB LSP pool thread ~7.
Found: TERNADDR r4 (box 7 run 2, 2026-10-31), PRE-EXISTING. Stopgap in TERNADDR: Windows stack headroom 4 MB, LSP pool
threads 64 MB on Windows, nesting legs sized for Windows.
Fix direction: split cold branches of ParsePostfixExpressionInner (and the next largest frames) into noinline helpers until
the Windows per-level cost is within ~2x of Mac; measure with dumpbin /unwindinfo before and after.
