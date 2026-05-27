# C Interop

> Part of the CFlat documentation set. See [`LANGUAGE.md`](LANGUAGE.md) for the language reference.

CFlat links against C in two ways: it can **compile your own `.c` source** (via `clang-cl`, merged by `lld-link`), or **bind a prebuilt library** by extracting its header and linking its import lib. Both require `-o` (linking is what merges the C side into the final image).

The bundled `clang-cl.exe` / `lld-link.exe` are deployed next to `cflat.exe`, so C interop is self-contained and needs nothing on `PATH`.

## Table of Contents

- [Compiling a `.c` File (auto-extern)](#compiling-a-c-file-auto-extern)
- [Importing a Program (`import program`)](#importing-a-program-import-program)
- [Binding a Prebuilt C Library (`import package`)](#binding-a-prebuilt-c-library-import-package)
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
import package "curl/curl.h" lib "libcurl.lib";                 // + import lib
import package "curl/curl.h" lib "libcurl.lib" define "CURL_STATICLIB" define "FOO=1";
```

- The header is resolved against the `--c-include` roots. A bare `import "curl/curl.h";` (any `.h/.hpp/.hh` extension) routes to the same path.
- The inline `lib` clause names an import library to link (resolved relative to the importing `.cb` if relative); it applies to that line only.
- One or more inline `define` clauses add preprocessor defines scoped to that header's AST dump, appended on top of the CLI `--c-define`. `lib` and `define` are soft keywords and must come after the string, `lib` before `define`.

The compiler AST-dumps a stub that `#include`s the header and registers the externally-linkable function **declarations** plus **enum constants** (as bare globals, matching C's flat enum scope - `CURLOPT_URL`, not `Type.CURLOPT_URL`):

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

A source-location filter keeps only declarations under the `--c-include` roots / the header's own directory, so the transitively-included SDK/CRT is excluded. Variadics are supported; `#define`-only constants are **not** (there is no macro in the AST). After a successful link, a sibling runtime DLL of each `--c-lib` is auto-copied next to the exe.

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
- **Install + resolution.** `vcpkg install --triplet <triplet>` runs in the manifest directory; the include dir, every `.lib` under `vcpkg_installed/<triplet>/lib`, and every `.dll` under `<triplet>/bin` belonging to the port closure are wired into clang-cl and `lld-link`. The DLLs are copied next to the produced `.exe` from the authoritative vcpkg list (deduped against the legacy `--c-lib` sibling-DLL probe).
- **LSP mode.** The `vcpkg install` subprocess is gated off when the compiler is running as the language server, so keystroke-driven analyses never spawn vcpkg.

The triplet defaults from `--platform` (`x64` -> `x64-windows`, `x86` -> `x86-windows`) and can be overridden:

| Flag | Description |
|------|-------------|
| `--vcpkg-exe <path>` | Explicit `vcpkg.exe`; bypasses VS-bundled / `VCPKG_ROOT` / `PATH` discovery |
| `--vcpkg-manifest <path>` | Explicit `vcpkg.json`; skips the upward walk |
| `--vcpkg-triplet <triplet>` | Override the platform-derived default (e.g. `x64-windows-static`) |

> `import package-vcpkg` and `import package` push into the same internal accumulators - you can mix a vcpkg-resolved port with a hand-pathed `--c-lib` in the same build. Defines from a vcpkg port's documented usage are applied automatically; add extra ones with `--c-define`.
