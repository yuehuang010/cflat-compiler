# CFlat examples

Runnable CFlat programs, grouped by the part of the language or platform they exercise.
Each folder is self-contained; most files carry a build/run line in their header comment.

Build them all at once with [`example.bat`](../example.bat) (Release by default), which
compiles every runnable `.cb` here, skips the library/helper files, and supplies the
per-folder import paths automatically:

```bash
example.bat            # Release
example.bat Debug
```

To build one by hand:

```bash
x64/Release/cflat.exe example/graphics/raytracer.cb -o out/raytracer.exe
```

| Folder | Contents | Platform |
|--------|----------|----------|
| [COM/](#com) | COM and WinRT through header/winmd-imported vtables | Windows |
| [graphics/](#graphics) | A CPU raytracer | Any |
| [hpc/](#hpc) | Numerical kernels, threading, and terminal visualizations | Any |
| [macos/](#macos) | Objective-C runtime, Cocoa, and Mach-O framework linking | macOS |
| [restAPI/](#restapi) | An HTTP/REST client-server stack written in CFlat | Any |
| [shell/](#shell) | Console utilities, an interactive shell, and terminal games | Any |
| [tools/](#tools) | Self-contained tools: compression, an interpreter, JSON config | Any |
| [ui/](#ui) | The `ui_native` UI framework tutorial (has its own README) | Any |
| [vcpkg/](#vcpkg) | Binding third-party C libraries through vcpkg | Windows |
| [windows/](#windows) | Win32 C interop: GUI, processes, files, sockets, registry | Windows |

## COM

Calling COM and WinRT objects with no hand-modeled vtables: the interfaces come straight
out of the Windows SDK headers (`import "d3d11.h"`) or Win32 metadata
(`import "Windows.Win32.winmd"`), and CFlat raises each one into a struct whose `lpVtbl`
field is the real vtable.

- `imalloc_demo.cb` - the smallest possible COM call, headless.
- `dxgi_adapters_demo.cb` - enumerate graphics adapters through DXGI.
- `d3d11_triangle_demo.cb` - render one triangle offscreen on the GPU and read it back.
- `d3d11_cube_demo.cb` - a real-time, depth-tested spinning cube in a window.
- `direct2d_demo.cb` - a Direct2D window whose interfaces come from winmd (needs `-i <winmd dir>`).
- `winrt_async_demo.cb` - awaiting `IAsyncOperation<T>`.
- `winrt_foreach_demo.cb` - `for (T x in iface)` over a live `IIterable<T>`.
- `winrt_ireference_demo.cb` - parameterized WinRT interfaces (derived PIIDs).
- `weather_cli_demo.cb` - WinRT `HttpClient` GET plus `json.cb` parsing, end to end.

## graphics

- `raytracer.cb` - a recursive CPU raytracer: spheres over a checkered plane, rendered to
  a BMP with operator-overloaded `vec3` math and one reflection bounce.

## hpc

Numerical work driving the `core/hpc/*` libraries (dense/sparse matrices, solvers, vector
math) plus the thread pool. Several render live to the terminal.

- `nbody.cb` / `nbody2.cb` - O(N^2) gravitational N-body, batch and realtime colored versions.
- `lu_bench.cb` - dense LU factorization benchmark.
- `poisson_cg.cb` - 2D Poisson solved with a sparse CSR matrix and conjugate gradient.
- `heat_stencil.cb` - 2D heat-equation stencil with an animated ANSI display.
- `spectrum.cb` - realtime FFT spectrum analyzer with a truecolor waterfall.
- `mc_pi.cb` - parallel Monte Carlo estimation.
- `fp_trap_demo.cb` - per-thread floating-point traps. Faults by design; run it manually.

## macos

Apple-platform interop. See also `ui/04-native-controls` and `ui/05-gallery` for Cocoa
under the UI framework.

- `hello_objc.cb` - calling Foundation and the Objective-C runtime from the console.
- `cocoa_window.cb` - a native AppKit window with a label, text field, and buttons.
- `cocoa_probe.cb` - a bridge-ABI probe for the typed `objc_msgSend` casts.
- `framework_link.cb` - `import framework` Mach-O linking.
- `sysinfo_mac.cb` - `sysctl` / libproc / Mach time through plain `extern` declarations.

## restAPI

A complete HTTP/1.1 and REST stack written in CFlat, plus its tests.

- `network/` - the library: parser, response builder, router, connection I/O, JSON body
  helpers, a single-handler `HttpServer`, a routed `RestServer`, and a sync client. Each
  server owns a `ThreadPool` inside its `program` scope.
- `todo_api.cb` - the showcase app: an in-memory Todo CRUD service.
- `benchmark.cb` - throughput benchmark, N client threads against the server.
- `test_*.cb` - unit and integration tests for the stack (`test_helper.cb` is shared, not run directly).

Build with `-i example/restAPI -i example/restAPI/network`.

## shell

Console programs. The coreutils-style ones (`cat`-alikes and friends) are one file each and
take arguments like their cmd.exe counterparts:

- Utilities: `cls`, `copy`, `del`, `dir`, `echo`, `mkdir`, `move`, `pwd`, `ren`, `rmdir`,
  `type`, `wc`, `where`.
- `fsh.cb` - Flat Shell: an interactive shell with a line editor, history, and pipes.
- Terminal games and graphics on `core/terminal.cb`: `tetris.cb`, `minesweeper.cb`,
  `missile_defender.cb` (mouse-driven), plus `stars.cb` and `bitmap.cb`, which need a
  Sixel-capable terminal (Windows Terminal 1.22+).

## tools

- `huffman.cb` - a Huffman compressor/decompressor.
- `interp.cb` - a tiny scripting language: lexer, parser, tree-walking evaluator
  (`demo.script` is sample input).
- `json_config.cb` - a full JSON round-trip via `JsonBuilder.fromJson<T>` (`config.json`,
  `sample.txt` are its inputs).

## ui

The `ui_native` framework tutorial, in numbered chapters that build on each other - element
model, terminal host, Win32 canvas, native OS controls, the widget gallery, WinUI 3,
automated UI testing, the `fedit` editor, and an interactive map. Read
[`ui/README.md`](ui/README.md); the reference is [`doc/UI.md`](../doc/UI.md).

## vcpkg

Binding prebuilt C libraries with `import package-vcpkg`, which runs `vcpkg install` against
the sibling `vcpkg.json` and resolves include dirs, link libs, and runtime DLLs itself.

- `zlib_demo.cb` - compress and decompress with zlib.
- `sqlite_demo.cb` - create, insert, and query a SQLite database.
- `get.cb` - an HTTP GET with libcurl.
- `sdl3_demo.cb` - an SDL3 window with a bouncing rectangle.
- `blas_gemm.cb` - OpenBLAS `cblas_dgemm` benchmarked against the core `Mat.gemm` kernel.

## windows

Win32 C interop driven by `import "windows.h"` - the SDK include and lib paths are found
automatically, so no `--c-include` flag is needed.

- GUI: `win_red_button.cb` (the canonical example), `clock_gdi.cb`, `minesweeper_gui.cb`,
  `bejeweled.cb`.
- System: `sysinfo.cb`, `process_explorer.cb` (tasklist clone), `vmmap.cb` (address-space
  walker), `enum_windows.cb`, `registry_dump.cb`.
- Files and I/O: `dirwatch.cb` (ReadDirectoryChangesW), `mmap_hexdump.cb` (memory-mapped
  hexdump), `pipe_echo.cb` (overlapped named pipes), `run_capture.cb` (child process stdout
  capture), `console_raw.cb` (raw key input), `tcp_http_get.cb` (blocking sockets).
