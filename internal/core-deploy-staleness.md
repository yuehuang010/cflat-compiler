# Core .cb deploy staleness: tests validate the DEPLOYED copy, not the source tree

## The trap

The compiler resolves `runtime.cb` (and the whole standard library) **next to the
executable** - `x64/Release/core/` or `x64/Debug/core/` - not from `cflat/core/`.
Those deployed copies are produced by the DeploymentContent items in `cflat.vcxproj`
**at build time**.

So this sequence silently tests the *old* standard library:

1. `msbuild cflat.slnx ...` (deploys core as a side effect)
2. edit `cflat/core/*.cb`
3. `test.bat` - **all green**, but it ran against the pre-edit core

This actually happened during the os.windows namespacing work: the full suite passed
against the stale deploy while the edited core had a real bug (a namespace function
signature referencing a sibling struct, only caught later by `cflat.exe --init`,
which compiles each core library standalone).

## The rule

After **any** edit to `cflat/core/*.cb`, rebuild via the solution (the incremental
build just re-copies content) **before** running `test.bat` / `example.bat`:

```bash
msbuild cflat.slnx -p:Configuration=Release -p:Platform=x64
test.bat Release
```

Two cheap detectors when results look suspicious:

- `--symbol <something-you-just-changed>` prints the source path it indexed; if it
  says `x64\Release\core\...` with a stale line number, the deploy is old.
- `diff cflat/core/foo.cb x64/Release/core/foo.cb`

`cflat.exe --init` is also a good standalone smoke for core edits: it compiles all
core libraries to bitcode one by one and fails fast on a library that only worked
because of import-order luck.
