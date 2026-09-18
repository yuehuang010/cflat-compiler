p2

# `Test/test_cpp_interop.cb` exits 139 under `--run`, while the same program as a native exe passes

Found 2026-09-17 during fix/cpp-const-ref-scalar. Not caused by that change: the file exits 139
under `--run` with that branch's new section removed as well, and the SAME source compiled with
`-o` runs to completion and returns 0.

## Repro

```
x64/Release/cflat Test/test_cpp_interop.cb -i Test/library --run     # 139
x64/Release/cflat Test/test_cpp_interop.cb -i Test/library -o out/i  # then ./out/i -> 0
```

`Test/test_cpp_interop_template.cb` and `Test/test_cpp_interop_bridge.cb` both exit 0 under
`--run`, so it is specific to this file's content.

PRE-EXISTING, measured on the master binary (2026-09-17, review round 1): at master 97773655,
`x64/Release/cflat Test/test_cpp_interop.cb -i Test/library --run` also exits 139, with none of
the fix/cpp-const-ref-scalar legs present. Not attributable to that change.

`lldb -b -o run` stops with `EXC_BAD_ACCESS (code=1, address=0x1084be330)` in a frame with no
symbol and unreadable surrounding memory - i.e. inside JIT-compiled code, not in the compiler.

## Why it matters even though the suite is green

`test.sh` / `test.bat` compile each test to a binary and run that, so the suite cannot see this.
`--run` is a supported mode (`test.sh --run` is an opt-in smoke set) and it is the mode agents
reach for when probing a repro, so a silent 139 here makes every `--run` probe over this fixture
untrustworthy.

## Investigation direction

The native image and the JIT get the same module, so suspect SYMBOL RESOLUTION rather than IR:
the generated `extern "C"` C++ helpers are now `__attribute__((weak))` and
`LinkCxxCompanionModules` demotes weak companion-origin definitions to internal after the link
(record 2026-09-16 in `internal/fix-issue-lessons.md`). A weak definition that the native linker
resolves and the JIT's resolver leaves null would call address 0-ish garbage exactly like this.
Bisect the file by deleting sections until `--run` stops crashing, then read the JIT'd module's
symbol table for the section that brings it back.
