# CMake core deploy merge-copies and never prunes, so renamed/deleted core files persist next to the exe

Summary
-------
`CMakeLists.txt` (~lines 247-264) deploys the core library tree with
`${CMAKE_COMMAND} -E copy_directory "${CFLAT_DIR}/core" "$<TARGET_FILE_DIR:cflat>/core"`.
`copy_directory` is a MERGE copy: it never deletes destination files that no longer
exist in the source. Rename or delete a `core/*.cb` and the old file lives on in
`x64/<Config>/core/` indefinitely.

This matters because `cflat.exe` reads the DEPLOYED core copy next to it, not the
source tree. A stale deployed file means builds and tests can pass against a module
that no longer exists in source - passing for the wrong reason.

Repro
-----
```
git mv cflat/core/foo.cb cflat/core/bar.cb
./cmake_build.bat release
ls x64/Release/core/          # both foo.cb AND bar.cb are present
```

Found during the 2026-07-09 UI host reorganization: after moving six modules into
`core/ui_native/` and `core/ui_canvas/`, a full rebuild left all six old flat
filenames in `x64/Release/core/` beside the new subdirectories. Any source file still
importing an old name would have compiled cleanly.

Note `x64/Debug/core/` and `x64/Release/core/` drift independently - rebuilding one
config does not clean the other.

Workaround
----------
Delete the deployed tree and the stamp, then rebuild:

```
rm -rf x64/Release/core x64/Debug/core
find build -name 'deploy_core.stamp' -delete
./cmake_build.bat release
```

Fix direction
-------------
Prune before copying, inside the same custom command that writes `deploy_core.stamp`
(so it still re-runs whenever any core file changes):

```cmake
COMMAND ${CMAKE_COMMAND} -E rm -rf "$<TARGET_FILE_DIR:cflat>/core"
COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${CFLAT_DIR}/core" "$<TARGET_FILE_DIR:cflat>/core"
```

`cmake -E rm -rf` requires CMake 3.17+. Verify the added delete does not defeat the
stamp's up-to-date check (the stamp depends on `CORE_FILES`, so an unchanged build
should not re-run the command at all). Confirm on all three platforms - the deploy
step is shared by the Windows, Linux, and macOS presets.

Related
-------
- The "rebuild before testing via the exe" rule exists because of this deploy
  indirection; this issue is the sharper edge of the same trap.
