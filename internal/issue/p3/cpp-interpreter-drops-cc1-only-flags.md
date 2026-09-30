# C++ import interpreter silently drops cc1-only flags

Found by the H1 review (perf timebox 2026-09-29). This is on master.

InterpreterArgs unwraps `-Xclang`, so the cc1-only flags `-disable-llvm-passes` and `-target-feature=+x` reach the Interpreter's driver as driver flags and are dropped. The second is also malformed for cc1, which expects the value as a separate argument. The comment in BuildCxxRequestClangArgs says LLVM passes are disabled; in fact they are not. H1 only makes the drop explicit: it filters the flags the driver does not know, to avoid spelling suggestions.

## Fix direction
Pass cc1-only flags through the Interpreter's cc1 argument hook (or keep them as `-Xclang` pairs the driver forwards), and spell `-target-feature` as two arguments. Then measure whether disabling passes changes request chunk time. Also fix the misleading comment.
