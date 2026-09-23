# `--check` refuses a header-inline member of an extern-template class that a real compile binds

Found 2026-09-22 (fix/cpp-std-streams) by the `--check` sweep over `Test/test_*.cb`.

## Repro

```cflat
import cpp {"sstream", "cctype"};   // any group not yet in the header cache
extern int main() { std.ostringstream o = default; std.string s = o.str(); return 0; }
```

`cflat chk.cb --check` -> `no overload of 'str' matches the given arguments.` `-v` shows
`C++ member std.ostringstream.str not bound: has no definition cflat can reach: clang emitted no
body for it`. The same file compiles and runs with `-o`. It reproduces identically on the pre-fix
binary (e3efe6cb), so it is not a streams-fix regression. It surfaces for
`Test/test_cpp_interop.cb` since its stream section (3200-3211) calls `o.str()`. Whether a warm
header cache hides it depends on which mode wrote the entry.

## Root cause (hypothesis, not traced)

In check mode `emitDefinitions` is false. `assumeInlineDefinitions` clears `needsLocalDefinition`
only for `md->isInlined() || isImplicit() || isDefaulted()` (CClangExtract.cpp ~1925). libc++'s
`basic_ostringstream<char>` is an `extern template` specialization, and `str()` is a
hide-from-ABI member of that specialization. It is most likely never instantiated and so not
`isInlined()`, which means the assumption skips it.

## Fix direction

Make the check-mode assumption match what the emission pass would prove. Read the member's
pattern decl, or instantiate the declaration without emitting it. Acceptance: the repro passes
`--check` cold, and the `--check` sweep over every `Test/test_*.cb` has only the Windows-only
files nonzero on macOS.
