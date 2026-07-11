# Core .cb deploy staleness: tests validate the DEPLOYED copy, not the source tree

## The trap

The compiler resolves `runtime.cb` (and the whole standard library) **next to the
executable** - `x64/Release/core/` or `x64/Debug/core/` - not from `cflat/core/`.
Those deployed copies are produced by the `deploy_core` custom target in
`CMakeLists.txt` (see the `deploy_core.stamp` custom command around line 249),
which runs as part of the normal build.

The `deploy_core` stamp depends on every file under `cflat/core/` (via a
`CONFIGURE_DEPENDS` glob), not on the `cflat` exe relinking - so editing only a
core `*.cb` (no C++ change) still triggers a re-copy on the next build. This
closes the historical trap where a POST_BUILD-only deploy step would leave the
deployed copy stale after a core-only edit; you still need to **rebuild** for
the copy to happen, but a plain `cmake_build.bat` run is enough - no C++ touch
required.

So this sequence used to silently test the *old* standard library (under the
legacy MSBuild build, which deployed core via `DeploymentContent` items in a
`.vcxproj` that has since been removed):

1. build (deploys core as a side effect)
2. edit `cflat/core/*.cb`
3. `test.bat` - **all green**, but it ran against the pre-edit core

This actually happened during the os.windows namespacing work: the full suite passed
against the stale deploy while the edited core had a real bug (a namespace function
signature referencing a sibling struct, only caught later by `cflat.exe --init`,
which compiles each core library standalone).

## The rule

After **any** edit to `cflat/core/*.cb`, rebuild **before** running `test.bat` /
`example.bat`, so the `deploy_core` target re-copies the tree:

```bash
./cmake_build.bat release
test.bat Release
```

Two cheap detectors when results look suspicious:

- `--symbol <something-you-just-changed>` prints the source path it indexed; if it
  says `x64\Release\core\...` with a stale line number, the deploy is old.
- `diff cflat/core/foo.cb x64/Release/core/foo.cb`

`cflat.exe --init` is also a good standalone smoke for core edits: it compiles all
core libraries to bitcode one by one and fails fast on a library that only worked
because of import-order luck.
