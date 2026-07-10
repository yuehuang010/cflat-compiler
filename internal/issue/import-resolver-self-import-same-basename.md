# Import resolver probes the importing file's own directory first, so a same-basename import self-resolves

Summary
-------
`ResolveImportPath` (`cflat/LLVMBackend.cpp`, ~line 1121-1133) tries
`(directory of the importing file) / <name>` as its FIRST candidate, before
`sourceFileDir_`, before `importSearchDirs`, and before the `runtimeDir/core`
fallback. When a file's own basename equals the name it imports, that first
candidate resolves to the file itself and the compile dies with a circular import,
even though the intended target exists on a later search path.

Repro
-----
Place a file at `cflat/core/ui_native/cocoa.cb` containing a bare
`import "cocoa.cb";` intended to reach the objc bridge at `cflat/core/cocoa.cb`:

```
x64/Release/cflat.exe example/ui/cocoa_native_settings.cb --check
cocoa_native_settings.cb(3633,0): Circular import detected: cocoa.cb -> cocoa.cb
```

Not platform-specific: reproduces without `--platform macos`, and when compiling the
subdirectory file directly.

Root cause
----------
Local-directory-first probing is unconditional. It does not exclude the importing
file itself, so `X/foo.cb` importing `"foo.cb"` can never reach a `foo.cb` that
lives on another search path.

Why it is not visible today
---------------------------
Existing core subdirectory imports work only because their basenames do not collide:
`core/hpc/densemat.cb` imports `"hpc/parallel.cb"` (fully qualified), and
`core/graphic/bitmap.cb` bare-imports `"filesystem.cb"` (different basename), so the
local-directory probe misses and correctly falls through to the core root.

Impact
------
Blocked moving the macOS NativeHost backend into `core/ui_native/` as `cocoa.cb`
during the 2026-07-09 UI host reorganization. The backend deliberately stays flat at
`cflat/core/ui_native_cocoa.cb`; see the comment in `cflat/core/ui_native/host.cb`.
The other backends moved (`ui_native/{host,win32,winui}.cb`,
`ui_canvas/{term,win32}.cb`) because none of them collide.

Fix direction
-------------
In `ResolveImportPath`, skip the local-directory candidate when it resolves to the
importing file itself, and continue to the next search directory:

```cpp
// A file cannot import itself; fall through to the next search dir.
if (Exists(candidate) && !SameFile(candidate, importingFile))
    return candidate;
```

Use a real same-file comparison (canonicalized path, or the existing path-normalizing
helper) rather than a string compare, so `./cocoa.cb` vs `cocoa.cb` still matches.
Add a negative regression under `Test/errors/` only if self-import should stay an
error when the name resolves nowhere else; otherwise add a positive case where a
subdirectory file bare-imports a same-named file from a parent search dir.

Risk: any code that currently relies on importing-file-directory-first for a
same-basename file would change resolution. Grep for such cases before landing (there
are none in-tree today).
