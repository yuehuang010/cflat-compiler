# C Interop

> Part of the CFlat documentation set. See [`LANGUAGE.md`](LANGUAGE.md) for the language reference.

CFlat links against C in two ways: it can **compile your own `.c` source** (via `clang-cl`, merged by `lld-link`), or **bind a prebuilt library** by extracting its header and linking its import lib. Both require `-o` (linking is what merges the C side into the final image).

The bundled `clang-cl.exe` / `lld-link.exe` are deployed next to `cflat.exe`, so C interop is self-contained and needs nothing on `PATH`.

## Table of Contents

- [Compiling a `.c` File (auto-extern)](#compiling-a-c-file-auto-extern)
- [Importing a Program (`import program`)](#importing-a-program-import-program)
- [Binding a Prebuilt C Library (`import package`)](#binding-a-prebuilt-c-library-import-package)

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

Auto-extern covers the scalar+pointer subset of the extern ABI (`int`, sized ints, `float`/`double`, `bool`, pointers including `void*` and opaque structs). Struct/union *by value* and pointer-to-pointer-to-pointer are skipped (run with `-v` to see what was skipped). C `long` maps to 32-bit (Windows LLP64); `long long` maps to 64-bit.

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
