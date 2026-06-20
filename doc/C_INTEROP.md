# C Interop

> Part of the CFlat documentation set. See [`LANGUAGE.md`](LANGUAGE.md) for the language reference.

CFlat links against C in two ways: it can **compile your own `.c` source** (via `clang-cl`, merged by `lld-link`), or **bind a prebuilt library** by extracting its header and linking its import lib. Both require `-o` (linking is what merges the C side into the final image).

The bundled `clang-cl.exe` / `lld-link.exe` are deployed next to `cflat.exe`, so C interop is self-contained and needs nothing on `PATH`.

## Table of Contents

- [Compiling a `.c` File (auto-extern)](#compiling-a-c-file-auto-extern)
- [Importing a Program (`import program`)](#importing-a-program-import-program)
- [Binding a Prebuilt C Library (`import package`)](#binding-a-prebuilt-c-library-import-package)
- [Passing a CFlat function as a C callback](#passing-a-cflat-function-as-a-c-callback)
- [Binding via vcpkg (`import package-vcpkg`)](#binding-via-vcpkg-import-package-vcpkg)

---

## Compiling a `.c` File (auto-extern)

A `.c` input is treated as **real C** - it is compiled by clang, not parsed by the CFlat parser. When you bring one in, the compiler runs clang's AST dump and **auto-registers an `extern` for every externally-linkable function the `.c` defines**, so no hand-written prototypes are needed:

```c
// util.c  (real C, compiled by clang)
//   int c_add(int a, int b)  { return a + b; }
//   int c_square(int x)      { return x * x; }

import "util.c";          // auto-externs c_add / c_square

extern int main()
{
    if (c_add(40, 2) != 42)  return 1;
    if (c_square(7) != 49)   return 2;
    return 0;
}
```

Bring a `.c` in either via `import "util.c";` (resolved through the import search dirs) or as an extra positional input on the command line:

```bash
cflat.exe app.cb util.c -o app.exe
```

Hand-written `extern` declarations still work and take precedence over the auto-generated ones:

```c
import "util.c";
extern int c_add(int a, int b);   // explicit; first-writer-wins
```

Auto-extern covers the scalar+pointer subset of the extern ABI (`int`, sized ints, `float`/`double`, `bool`, pointers including `void*` and opaque structs). C `long` maps to 32-bit (Windows LLP64); `long long` maps to 64-bit. Pointer-to-pointer-to-pointer is skipped (run with `-v` to see what was skipped).

**Struct-by-value** is supported: when clang's AST dump exposes a `struct Foo { ... };` decl, cflat auto-registers `Foo` as a CFlat struct type (matching C field layout) and lowers calls that take or return `Foo` by value per the Win64 / Win32 MSVC ABI. Sizes `1, 2, 4, 8` bytes are coerced into an integer register (`i8`/`i16`/`i32`/`i64`); other sizes go through `byval` pointer (parameter) or hidden `sret` pointer (return). On Win32 the byval slot is reported at `align 4` to match the cdecl 4-byte stack alignment. Unions by value are not yet supported.

> A `.c` means real C. CFlat fixtures that historically used a `.c` extension now use `.cb`.

## Importing a Program (`import program`)

`import program "file" as Alias;` pulls another file's `main` in as a [`program`](LANGUAGE.md#program-keyword) - the imported entry point runs on its own managed thread. The file can be either a `.cb` or a real `.c`:

```c
import "list.cb";
import "thread.cb";

import program "worker.cb"        as Worker;   // CFlat main
import program "hello_program.c"  as HelloC;   // real C main(int argc, char** argv)

extern int main()
{
    HelloC h;
    list<string> args;
    args.add("" + "a");
    args.add("" + "b");

    h.run(args);
    h.WaitForExit();
    return h.exitCode;   // 2  (C main returns argc)
}
```

The imported program exposes the same synthetic fields as a normal `program` (`exitCode`, `onStdout`, ...). A C `main(int argc, char** argv)` is wired into the program runtime just like a CFlat `main(move list<string> args)`.

## Binding a Prebuilt C Library (`import package`)

Unlike a `.c` (which cflat *compiles*), a prebuilt library (libcurl, zlib, ...) is consumed by extracting its **header** and linking its **import lib**. The source declares *what* to bind; the CLI (or inline clauses) supply *where* the machine-specific files live, keeping absolute paths out of source.

```c
import package "curl/curl.h";                                   // header only
import package "curl/curl.h" lib "libcurl.lib";                 // + one import lib
import "windows.h" lib { "user32.lib", "gdi32.lib" };           // + several import libs
import package "curl/curl.h" lib "libcurl.lib" define "CURL_STATICLIB" define "FOO=1";
```

- The header is resolved against the `--c-include` roots. A bare `import "curl/curl.h";` (any `.h/.hpp/.hh` extension) routes to the same path. The inline `lib` / `define` clauses work on this bare header form too - `import package` is only required when you also need an `as` alias-free header without a `.h` extension.
- The inline `lib` clause names the import library (or libraries) to link, and applies to that line only. Use the brace form `lib { "a.lib", "b.lib" }` when one header fans out to several import libs (common for `windows.h`). Resolution: a library that ships **beside the importing `.cb`** is linked by path (the Conan/vcpkg layout); a bare **system lib name** that is not found there (e.g. `user32.lib`) is passed through to lld-link, which resolves it against the system lib search paths - the same Windows SDK lib dir cflat already discovered to find the header. So `lib { "user32.lib", "gdi32.lib" }` needs no `--c-lib` and no absolute paths.
- One or more inline `define` clauses add preprocessor defines scoped to that header's AST dump, appended on top of the CLI `--c-define`. `lib` and `define` are soft keywords and must come after the string, `lib` before `define`.

The compiler AST-dumps a stub that `#include`s the header and registers the externally-linkable function **declarations**, **enum constants** (as bare globals, matching C's flat enum scope - `CURLOPT_URL`, not `Type.CURLOPT_URL`), and the header's **struct / union types**. A `typedef struct tag { ... } Name;` is usable under either name - the tag (`tagMSG`) or the typedef (`MSG`):

```c
import package "cinterop/mathlib.h" lib "cinterop/mathlib.lib" define "ML_EXTRA";

extern int main()
{
    if (ml_add(40, 2) != 42)            return 1;   // plain function
    if (ml_sum(4, 10, 20, 30, 40) != 100) return 3; // variadic
    if (WHITE != 100)                   return 4;   // enum constant
    if (EXTRA_FLAG != 7)                return 6;   // gated by the inline define
    return 0;
}
```

A source-location filter keeps only **functions / enum constants / macros** declared under the `--c-include` roots / the header's own directory, so the transitively-included SDK/CRT API surface is excluded. **Structs are an exception**: an in-scope struct often embeds a struct defined in a sibling SDK directory by value (e.g. `MSG` contains a `POINT`, which lives in the SDK `shared/` dir), so the compiler additionally registers the transitive by-value dependency closure of the in-scope structs. You therefore get `MSG`, `RECT`, `POINT`, `WNDCLASSEXA`, ... from `import "windows.h"` without hand-declaring them. **The Windows SDK `shared/` dir is another exception**: when the bound header sits in the SDK `um/` dir, the sibling `shared/` dir is treated as in-scope too (and vice versa), so the constants MSDN-style code reaches for immediately - `ERROR_SUCCESS` and the other `winerror.h` codes, `MAX_PATH`, HRESULTs like `E_FAIL` - fold without hand-transcribing. Variadics are supported. **Object-like `#define` constants are folded** and surfaced as bare globals (e.g. `WM_DESTROY`, `WS_OVERLAPPEDWINDOW`, `SW_SHOW` resolve to their numeric values), provided the macro body folds to an integer / float / string constant; a macro that does not fold to a constant (e.g. one that expands to a function name or token paste) is skipped. A folded integer macro keeps its **natural C type** (`((DWORD)-10)` registers as `u32`, not a width-guessed `i64`), so it passes to the corresponding API parameter without a cast, and a pseudo-handle cast macro (`HKEY_LOCAL_MACHINE`, `INVALID_HANDLE_VALUE`) folds to a `void*` constant. After a successful link, a sibling runtime DLL of each `--c-lib` is auto-copied next to the exe.

A C **function-pointer field** (e.g. `WNDPROC lpfnWndProc` in `WNDCLASSEXA`) is registered as a bare `void*`, since a C callback slot is a single pointer (CFlat's `function<T>` is a 16-byte closure and would corrupt the struct's ABI layout). Store a CFlat callback into such a field by casting the function's name: `wc.lpfnWndProc = (void*)WndProc;`. See [Passing a CFlat function as a C callback](#passing-a-cflat-function-as-a-c-callback).

A **fixed-size array struct field** (e.g. `CHAR szExeFile[260]` in `PROCESSENTRY32`) is registered as an inline CFlat array, not decayed to a pointer, so the C ABI `sizeof` is exact - `sizeof(PROCESSENTRY32)` is 304, not the 40 a decayed pointer would give. Only positive integer extents become inline arrays; an incomplete `[]` still decays to a pointer, and a field whose element type cannot be mapped abandons the whole record (an opaque shell that errors on use - never a silent size change).

#### Non-self-contained headers (grouped import)

Some headers are **not self-contained**: they use types (`HANDLE`, `DWORD`, ...) without including the header that defines them, relying on the translation unit having included a prerequisite first. The classic case is `tlhelp32.h`, which assumes `windows.h` was included before it. Bound on its own, clang cannot resolve the base types, drops every dependent function declaration, and the import fails with a loud diagnostic:

```
C header 'TlHelp32.h' does not compile on its own (unknown type name 'HANDLE'). It likely
needs a prerequisite header included first. Import them together as one group so they share a
single translation unit, e.g. import { "prerequisite.h", "TlHelp32.h" };
```

cflat has **no built-in knowledge** of which header is the prerequisite - that belongs in your source. Express the dependency with a **grouped import**: the C-header entries of a group are extracted as **one translation unit**, the stub `#include`s each entry in listed order, so an earlier entry satisfies a later one's prerequisites:

```c
import { "windows.h", "tlhelp32.h" } lib { "user32.lib", "gdi32.lib" };
```

Each header still registers only the decls that live in its own directory (the union of the group's header directories, plus the SDK `um/`<->`shared/` sibling expansion), so the group does not over-expose. Group-level `lib`, `define`, and `cache` clauses apply to the whole group; non-header (`.cb`/`.c`) entries in the same group route individually as usual. The grouped header set is folded into the cache key, so a header bound standalone never collides with the same header bound after a prerequisite.

### System headers (e.g. `windows.h`)

A bare `import "windows.h";` binds the system header with **no `--c-include` flag**: cflat auto-detects the latest installed Windows SDK include directory (`...\Windows Kits\10\Include\<ver>\um`, plus `shared`/`ucrt`/`winrt`) and uses it to resolve the header. The header's own directory then becomes the in-scope root, so its declarations are kept while the transitively-included CRT/MSVC headers are still filtered out. The parse itself relies on clang's in-process MSVC toolchain auto-detection, so the MSVC/UCRT/shared headers do not need to be named.

`kernel32.lib` is already on the default link line. Functions from other system libraries (e.g. `user32.lib` / `gdi32.lib` for a GUI app) are linked with the inline `lib` clause, naming the bare lib filename - cflat resolves it against the SDK lib dir it already discovered, so no `--c-lib` or absolute path is needed:

```c
import "windows.h" lib { "user32.lib", "gdi32.lib" } cache;
```

See `example/windows/sysinfo.cb` (console, kernel32 only) and `example/windows/win_red_button.cb` (a GUI window + owner-drawn red button, linking user32/gdi32 via the inline `lib` clause).

Core's own Win32/CRT bindings live in the `os.windows` namespace, so they never collide with the header bind: `import "windows.h"` registers its structs under the MSDN names (`CONSOLE_SCREEN_BUFFER_INFO` with nested `dwSize.X`, `COORD`, ...) while core code reaches its own flattened copies as `os.windows._CONSOLE_SCREEN_BUFFER_INFO` etc. A namespaced `extern` keeps its bare linkage symbol, so `os.windows.Sleep` and the header's bare `Sleep` resolve to the same imported function.

A record the extractor cannot fully map - e.g. `INPUT_RECORD`, which embeds an *anonymous union* - registers as an opaque, unsized shell: usable through a pointer, but declaring one **by value** is a compile error ("incomplete layout"). Use core's flattened `os.windows._INPUT_RECORD` for by-value console input records (see `example/windows/console_raw.cb`).

The matching CLI flags supply the machine-specific paths (all repeatable):

```bash
cflat.exe app.cb --c-include <inc-dir> --c-lib <path/to/lib.lib> --c-define CURL_STATICLIB -o app.exe
```

| Flag | Description |
|------|-------------|
| `--c-include <dir>` | Header search root; also passed as `-I` to the AST dump |
| `--c-lib <path>` | Prebuilt import library (`.lib`) added to the `lld-link` line |
| `--c-define <NAME[=val]>` | Preprocessor `/D` applied to **every** clang-cl spawn (header dump, `.c` auto-extern dump, `.c` object compile) |

> `--c-define` is process-wide; an inline `define` clause applies only to its own import line and is appended after the CLI defines.

## Passing a CFlat function as a C callback

C APIs frequently take a function pointer (a `qsort` comparator, a Win32 `WNDPROC`, ...). A C function pointer is a **bare code address**. CFlat's `function<T>` is a 16-byte `{code, env}` closure that can carry captured state, so the two are not interchangeable.

**Pass a non-capturing named function directly.** Declare the C parameter as `function<R(Args)>` and hand it the function's name - the compiler lowers the bare code address across the C ABI:

```c
extern void qsort(void* base, u64 num, u64 size, function<int(void*, void*)> cmp);

int byInt(void* a, void* b) { return *(int*)a - *(int*)b; }

extern int main()
{
    int[5] xs = {5, 3, 1, 4, 2};
    qsort(&xs[0], 5u, 4u, byInt);   // OK - named function passed directly
    return xs[0];                   // 1
}
```

**A lambda or a `function<>` variable is rejected at compile time** when passed to a C function-pointer parameter:

```c
qsort(&xs[0], 5u, 4u, (int a, int b) => { ... });   // error: cannot pass a lambda ...
function<int(void*,void*)> f = byInt;
qsort(&xs[0], 5u, 4u, f);                            // error: ... or 'function<>' variable ...
```

C cannot carry the closure's env slot, so allowing this would silently drop captured state and crash. Refactor the capture into explicit parameters / globals and pass a plain named function.

**Storing a callback in a C struct field** (e.g. `WNDCLASSEXA.lpfnWndProc`): the field is a bare `void*` (see above), so assign the function's address with a cast - `wc.lpfnWndProc = (void*)WndProc;`.

**`stdcall` callbacks.** A Win32 callback uses the `stdcall` (WINAPI) convention. Mark the CFlat function with the `stdcall` specifier so it matches on the 32-bit target (it is a no-op on x64, but keep it for portability):

```c
stdcall i64 WndProc(void* hwnd, u32 msg, u64 wParam, i64 lParam) { ... }
```

## Binding via vcpkg (`import package-vcpkg`)

`import package-vcpkg "<header>" from "<port>";` lets cflat do the machine-specific bookkeeping itself: it walks up from the importing `.cb` for a `vcpkg.json`, runs `vcpkg install` once, and pushes the resolved include dir, import libs, and runtime DLLs into the same accumulators the manual flags use. No `--c-include` / `--c-lib`, no sibling `build.bat`.

```c
// example/vcpkg/get.cb
import package-vcpkg "curl/curl.h" from "curl";

extern int main()
{
    void* curl = curl_easy_init();
    curl_easy_setopt(curl, CURLOPT_URL, "https://example.com");
    curl_easy_perform(curl);
    curl_easy_cleanup(curl);
    return 0;
}
```

The sibling `vcpkg.json` is the source of truth for what is allowed to be imported:

```json
{
  "name": "cflat-vcpkg-example",
  "version-string": "0.0.1",
  "dependencies": [ "curl" ],
  "builtin-baseline": "d015e31e90838a4c9dfa3eed45979bc70d9357fc"
}
```

How it resolves:

- **Manifest discovery.** The compiler walks up from the source file's directory until it finds a `vcpkg.json`. The walk stops at a `.git` directory so it cannot escape the project root. If none is found, the compiler errors with a `vcpkg new --application` hint.
- **Port validation.** Every `from "<port>"` must appear in the manifest's `dependencies`. A missing port produces a copy-pasteable `vcpkg add port <name>` command and aborts the build - the manifest stays authored by the user, not synthesized by the compiler.
- **vcpkg.exe discovery.** Order: `--vcpkg-exe`, `VCPKG_ROOT`, the VS-bundled `<VS>\VC\vcpkg\vcpkg.exe` (located via `vswhere`), then `vcpkg.exe` on `PATH`.
- **Install + resolution.** `vcpkg install --triplet <triplet>` runs in the manifest directory; the include dir, every `.lib` under `vcpkg_installed/<triplet>/lib`, and every `.dll` under `<triplet>/bin` belonging to the port closure are wired into clang-cl and `lld-link`. The DLLs are copied next to the produced `.exe` from the authoritative vcpkg list (deduped against the legacy `--c-lib` sibling-DLL probe). The install is watermark-gated (re-runs only when `vcpkg.json` is newer than `vcpkg_installed/vcpkg/status`), so incremental builds don't re-spawn vcpkg.
- **Skipping the install.** `--vcpkg-no-install` suppresses the `vcpkg install` spawn for a CLI build: cflat consumes an already-populated `vcpkg_installed/<triplet>/` tree and **errors out** if the port isn't present (useful for offline / locked-down or CI builds where packages are pre-provisioned). This is the same skip the LSP always applies, but in CLI mode a missing package is a hard error rather than a silent skip.
- **LSP mode.** The `vcpkg install` subprocess is gated off when the compiler is running as the language server, so keystroke-driven analyses never spawn vcpkg. A package that isn't installed yet is skipped silently (no error diagnostic) - the symbols simply stay unindexed until a CLI build populates the tree.

Like `import package`, a vcpkg import accepts inline `define` clauses scoped to that header's clang bind (same effect as `--c-define`, layered on top of it):

```c
import package-vcpkg "SDL3/SDL.h" from "sdl3" define "SDL_MAIN_HANDLED";
import package-vcpkg "x.h" from "port" define "FOO" define "BAR=2";   // repeatable
```

The triplet defaults from `--platform` (`x64` -> `x64-windows`, `x86` -> `x86-windows`) and can be overridden:

| Flag | Description |
|------|-------------|
| `--vcpkg-exe <path>` | Explicit `vcpkg.exe`; bypasses VS-bundled / `VCPKG_ROOT` / `PATH` discovery |
| `--vcpkg-manifest <path>` | Explicit `vcpkg.json`; skips the upward walk |
| `--vcpkg-triplet <triplet>` | Override the platform-derived default (e.g. `x64-windows-static`) |
| `--vcpkg-no-install` | Do not run `vcpkg install`; consume the existing `vcpkg_installed/` tree and error out if a port is missing |

> `import package-vcpkg` and `import package` push into the same internal accumulators - you can mix a vcpkg-resolved port with a hand-pathed `--c-lib` in the same build. Defines from a vcpkg port's documented usage are applied automatically; add extra ones with an inline `define` clause or `--c-define`.
