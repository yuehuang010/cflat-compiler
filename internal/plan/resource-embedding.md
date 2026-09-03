# Resource embedding (IMPLEMENTED 2026-09-03; Windows paths unverified)

Ratified and implemented 2026-09-03 on macOS: steps 1-4 of section 5 landed (`embed(...)`,
`application` declaration, `--dump-app-info`, Windows `.res` writer, macOS `__TEXT,__info_plist`
and `-o App.app` bundle, `Application.*` API, ui_native auto icon). Byte serializers live in
`cflat/AppResources.{h,cpp}`. Windows-only paths (lld-link `.res` consumption, `LoadIconA`,
`WM_SETICON`, `Test/test_windows.cb`, `win32_native_settings.cb`) were written but not executed:
verify on the Windows host. Step 5 (Linux `.desktop`) remains a separate plan.

**Open on the Windows box: user32.lib without the SDK.** `Application.icon()` calls
`LoadIconA` (user32), and `ui_native/win32.cb` needs user32/gdi32/comctl32/shell32/ole32/
comdlg32/dwmapi/uxtheme. Today those resolve only through the Windows SDK `um` fallback
dir; `--init` synthesizes import libs from System32 DLLs for kernel32/ws2_32/ntdll/dbghelp/
advapi32/ucrtbase only (`SynthesizeSystemImportLibs`, `cflat/LLVMBackend.cpp`). Investigate:
(a) add the GUI DLLs to that table (cheapest, host-only); (b) Zig's route - ship mingw-w64
`.def` files and synthesize from them, SDK-free and cross-compile-capable (check the
mingw-w64 runtime license before vendoring); (c) a declaration-side `dll "user32.dll"`
clause on `extern stdcall` so a core `.cb` can request the import lib without importing
`windows.h` (grammar + both ParseDeclarationSpecifiers copies; needs a `p4/` ruling).
Zig requires the SDK for its msvc target and is SDK-free only for windows-gnu. Maintainer
constraints recorded: cross-platform solution, not a Windows RC port; no additional file
extensions (no `.rc`, no `.qrc`-style manifest file).

## Motivation (from the closed issue)

Filed 2026-08-21 from the MemPressMonitor Win32 port (v0.11.0 issue 09); the reporter named
the missing `rc.exe` equivalent the LARGEST single porting cost of the exercise:

- The dialog-based main window was rebuilt as a hand-registered window class -
  re-implementing `WM_ERASEBKGND` painting and `WM_SETFONT` broadcast that the dialog manager
  gives for free (the `.rc` line `FONT 9, "Segoe UI"` became a hand-written
  `_applyControlFont()`). Both omissions were silent, caught only by eyeballing screenshots.
- The app icon shipped as a generated `u8[6960]` array plus a hand-written `ICONDIR` walker
  feeding `CreateIconFromResourceEx`, plus a build-script step to regenerate the array.

## 1. What the Windows RC system actually does

`.rc` -> `rc.exe` -> `.res` -> linker -> PE `.rsrc` section. One mechanism, six unrelated jobs:

| Job | RT_ type | Consumer | Category |
|-----|----------|----------|----------|
| Arbitrary bytes by type+id/name | RT_RCDATA, custom | `FindResource`/`LoadResource`/`LockResource` in the program | **embed** |
| App icon, cursors, bitmaps | RT_GROUP_ICON, RT_ICON, RT_CURSOR, RT_BITMAP | Explorer (first icon group), `LoadIcon`/`LoadImage` | **app metadata** + embed |
| Version block | RT_VERSION | Explorer properties, installers, crash telemetry | **app metadata** |
| SxS manifest | RT_MANIFEST | loader at process start | **app metadata** (DONE: `manifest` decl) |
| String table | RT_STRING | `LoadString`, per-LANGUAGE localization, MUI satellites | localization |
| Dialog / menu / accelerator templates | RT_DIALOG, RT_MENU, RT_ACCELERATOR | dialog manager (`DialogBox`, `LoadMenu`, `LoadAccelerators`) - DLU layout, tab order, `FONT` broadcast | declarative UI |
| Message tables, typelibs, HTML | RT_MESSAGETABLE, TYPELIB, RT_HTML | `FormatMessage`, COM, res:// | niche |

Key property: the data lives INSIDE the executable image and is addressable by an external
tool or another process, not just by the program itself. That is why the shell can show the
icon of a bare `.exe`.

## 2. Equivalents on Linux and macOS

**Linux (ELF).** No resource section, no shell metadata in the binary.

- Bytes: `ld -r -b binary` / `objcopy`, `xxd -i`, C23 `#embed`, Rust `include_bytes!`, Go
  `//go:embed`. Result is an ordinary symbol in `.rodata`. GLib's GResource
  (`glib-compile-resources`, XML manifest -> C array, `/org/app/x` paths) and Qt `.qrc` ->
  `rcc` are the closest "resource compiler" analogues - both are just embed-by-path.
- Icon: NOT in the binary. `.desktop` entry + icon theme dir (`share/icons/hicolor/<size>/apps/`),
  or AppImage/Flatpak packaging. A bare ELF has no icon, ever.
- Version: none standard. `--version`, package metadata (deb/rpm).
- Strings: gettext `.mo` catalogs on disk under `LC_MESSAGES`.
- Dialogs/menus: GtkBuilder `.ui` / Qt `.ui` XML, in files or embedded via GResource.

**macOS (Mach-O).** The `.app` bundle is the resource system; the executable carries almost nothing.

- Bytes: `-sectcreate SEG SECT file` linker flag + `getsectiondata()` is the in-binary path;
  `NSBundle pathForResource:` is the bundle path.
- Icon: `Contents/Resources/App.icns`, named by `CFBundleIconFile` in `Info.plist`. Bare
  Mach-O = generic icon.
- Version / identifier: `Info.plist` (`CFBundleShortVersionString`, `CFBundleVersion`,
  `CFBundleIdentifier`). A BARE executable can carry an Info.plist in the
  `__TEXT,__info_plist` section (Apple-documented for single-file tools; codesign and
  LaunchServices read it). This is the only Mach-O analogue of RT_VERSION/RT_MANIFEST.
- Strings: `<lang>.lproj/Localizable.strings`, `NSLocalizedString`.
- Dialogs/menus: `.xib`/`.storyboard` -> `.nib`, loaded by `NSNib`.

**Conclusion.** Only ONE thing is portable at the binary level: "bytes in the image, addressable
by name". Icon/version/identifier are OS-shell metadata and each OS wants them in a different
container (PE `.rsrc` / `.desktop`+theme dir / `Info.plist`+`.icns`). Dialog templates are a
GUI-framework concern, not a resource concern. The design separates these three.

## 3. CFlat design

Three layers. Layer 3 is explicitly NOT built by this plan.

### 3.1 `embed(...)` - portable bytes (the primitive)

Compile-time builtin EXPRESSION, like `sizeof` / `expect_error`: takes one string literal
path, folds to a constant. Not a declaration specifier, not a keyword - so no
`ParseDeclarationSpecifiers` change and no reserved word. Modeled on C23/C++26 `#embed` and
Rust `include_bytes!`, both expression-position; an expression composes into function
arguments, brace-init, and the `application` icon list below.

```cflat
const u8[] payload = embed("data/payload.bin");      // u8[N], N = file size
const string blitShader = embed("shaders/blit.glsl"); // string, byte-exact tracked length
```

Semantics:
- Path resolves relative to the DECLARING source file (not cwd, not the import path).
  Missing file, or a path escaping above the import root with `..`, is a `LogError` at the
  expression.
- Result type from context: `u8[N]` by default, `string` when the target type is `string`.
  Any other target type is an error naming both allowed spellings. `u8[N]` count comes from
  the file, so `sizeof` and the fixed-array count machinery (see raw-array-count desugar
  direction) work unchanged. Text needs no NUL suffix - `string` carries its length.
- Storage is an ordinary LLVM constant global in the read-only data section
  (`.rdata` / `.rodata` / `__TEXT,__const`) on EVERY target. NOT `RT_RCDATA`: identical on
  all three targets, no OS API to read, works under `--run` (JIT has no PE image) and when
  cross-compiling. Nothing outside the program needs to find a blob, so `.rsrc` buys nothing.
- Immutable: writes through the name are rejected like writes to a string literal.
- Recorded in `<out>.cflat-dep.json` with mtime so the up-to-date check (issue
  `p3/no-incremental-build-and-no-up-to-date-check.md`) invalidates on asset change.
- v1: one literal path, no globs, no directories, no `limit`, no existence probe.

Normal access pattern is the C one - pointer plus length into a consuming API - except the
length is carried by `string` / `view<u8>` instead of `sizeof - 1`:

```cflat
glShaderSource(sh, 1, &blitShader.data, &blitShader.length);
lib = device.newLibrary(view<u8>(payload));
```

This alone retires the `u8[6960]` array + regeneration script from the motivation.

**No `[section("...")]` attribute (ruling 2026-09-02).** C/C++ (`__attribute__((section))`,
`__declspec(allocate)`) and Rust (`#[link_section]`) have one, but its meaning is not
portable: on Mach-O a named section is how system tools find data; on ELF it is firmware /
kernel placement; on PE the equivalent role is the `.rsrc` directory tree, which a section
name cannot produce (no directory -> `FindResource`, Explorer, signtool all fail), and every
distinct COFF section name costs a file-aligned, page-aligned image section plus 8-char name
truncation and characteristics flags. The only named-section need identified
(`__TEXT,__info_plist`) is emitted internally by `application` on Mach-O. A future
Mach-O-only need (e.g. a metallib located by section) becomes its own `p4/` item.

### 3.2 `application` - OS-facing app metadata (one declaration, per-target routing)

Follows the `manifest` precedent exactly: soft specifier, typed by a core struct, compile-time
literal initializer, file scope. Types in `.cb`, literal walk in `MainListener`, byte layout in
the backend. Maintainer ruling 2026-09-02: typed declaration path, not CLI flags.

```cflat
import "application.cb";

application AppInfo app = {
    name        = "MemPress Monitor",
    identifier  = "com.example.mempress",
    description = "Memory pressure monitor",
    company     = "Example Inc",
    copyright   = "(c) 2026 Example Inc",
    version     = { file = "0.11.0" },
    icon        = { { image = embed("icon-16.png") }, { image = embed("icon-32.png") },
                    { image = embed("icon-256.png") }, { image = embed("icon-1024.png") } },
};
```

#### Types: `core/application.cb`

Plain structs, like `manifest.cb`, so the type checker validates the literal and the LSP gets
hover/completion for free. The struct IS the schema.

```cflat
enum FileType : int { application, dll, driver };

struct AppIcon {
    u8[] image = default;        // one embed("x.png"); or a single .ico / .icns escape hatch
};

struct AppVersion {
    string file = default;       // "0.11.0": FileVersion string + VS_FIXEDFILEINFO dwFileVersion
    string product = default;    // ProductVersion; defaults to file
    bool prerelease = default;   // VS_FF_PRERELEASE
    bool debug = default;        // VS_FF_DEBUG
};

struct AppInfo {
    string name = default;         // ProductName            / CFBundleName
    string identifier = default;   // InternalName           / CFBundleIdentifier
    string description = default;  // FileDescription        / (none)
    string company = default;      // CompanyName            / (none)
    string copyright = default;    // LegalCopyright         / NSHumanReadableCopyright
    AppVersion version = default;  // RT_VERSION             / CFBundleShortVersionString, CFBundleVersion
    FileType type = default;       // dwFileType             / CFBundlePackageType
    list<AppIcon> icon = default;  // RT_GROUP_ICON+RT_ICON  / .icns + CFBundleIconFile
    int language = 1033;           // LanguageId on every .res entry + VarFileInfo Translation
};
```

Rules, checked at the declaration on any host (so `--check` on macOS catches them):
- Exactly ONE `application` declaration per program; two is an error naming both locations.
  Libraries (including `ui_native`) never declare one. No fragment merging - unlike
  `manifest`, nothing a library contributes is legitimate.
- `version.file` / `version.product`: one to four dotted decimals, each < 65536. Missing
  trailing components are zero.
- `icon`: every `image` is a PNG (signature + IHDR checked: square, sizes distinct, one of
  16/32/48/64/128/256/512/1024); OR the list has exactly one entry whose bytes are an `.ico`
  (`ICONDIR` reserved=0, type=1) or `.icns` (`icns` magic) - used as-is on its own platform,
  error at the declaration when building for the other. No new extensions: PNG, ICO, ICNS are
  what the OSes already demand.
- PNG set -> ICO / ICNS conversion is done by the COMPILER, no external tool: since Vista an
  ICO entry may be raw PNG, and since 10.7 ICNS `ic07`..`ic13` chunks are raw PNG. Both are a
  header plus the PNG bytes.
- `name` required when the declaration exists; everything else optional. Empty strings are
  omitted from the version block / plist, not emitted empty.

#### Where the pieces live

| Piece | Location |
|-------|----------|
| Struct declarations | `cflat/core/application.cb`, deployed next to the exe like every core file |
| Specifier + literal walk | sibling of the `isManifest` branch in `MainListener_Declarations.cpp`; records ONE `ApplicationFragment` with typed leaves (strings, ints, bools, embed byte spans) |
| Validation, one-decl rule | at the declaration, host-independent |
| Windows serialization | the `.res` writer in `LLVMBackend_EmitAndLink.cpp`, generalized from one manifest entry to a list of entries |
| macOS serialization | `WritePlistSection` sibling: `__TEXT,__info_plist` global, or bundle writer for `-o App.app` |
| Runtime API | `core/application.cb` namespace `Application` (below) |

#### Windows: `.res` layout

Unchanged mechanism: the compiler writes ONE temp `.res` and appends it to the lld-link
arguments (what the manifest does at `LLVMBackend_EmitAndLink.cpp` today). lld's
`WindowsResource` library converts it to `.rsrc$01/.rsrc$02` and builds the directory tree.
No rc.exe, no cvtres, no PE section work, no IR involvement; works when cross-compiling
from macOS.

A `.res` is a flat sequence of entries. Each entry = 32-byte header + payload, 4-byte aligned:

```
u32 DataSize, u32 HeaderSize(32), u16 0xFFFF + u16 TypeOrdinal, u16 0xFFFF + u16 NameId,
u32 DataVersion(0), u16 MemoryFlags(0x0030), u16 LanguageId(app.language),
u32 Version(0), u32 Characteristics(0)
```

That header is what the existing `appendHeader` lambda already writes. Entries emitted, in
order, after the mandatory empty leading entry:

| Entry | Type ordinal | Name id | Payload | Source |
|-------|--------------|---------|---------|--------|
| RT_ICON x N | 3 | 1..N (list order) | raw PNG bytes (or the DIB/PNG image sliced out of the user `.ico`) | `icon[i].image`, verbatim |
| RT_GROUP_ICON | 14 | 1 | `GRPICONDIR{0, 1, N}` + N x `GRPICONDIRENTRY{w, h, colors=0, reserved=0, planes=1, bitCount=32, bytesInRes, iconId}` | w/h from PNG IHDR (256+ encodes as 0), bytesInRes = embed length, iconId = 1..N; from `ICONDIR` entries when splitting a `.ico` |
| RT_VERSION | 16 | 1 | `VS_VERSIONINFO` (below) | `name`, `identifier`, ..., `version`, `type`, `language` |
| RT_MANIFEST | 24 | 1 | UTF-8 XML | existing `manifest` merge, unchanged |

Nothing is hardcoded except the four type ordinals and the id convention: user supplies
pixels, compiler supplies numbering and record layout, Win32 supplies ordinals. Group id 1 is
what Explorer shows (lowest-numbered group) and what `Application.icon()` loads. Ids 1..N and
group 1 are reserved; a linked `.c` object carrying its own resources must avoid them (lld
reports duplicates as a link error). No `icon` field -> no RT_ICON / RT_GROUP_ICON entries.

`VS_VERSIONINFO` payload, every block `{u16 wLength, u16 wValueLength, u16 wType, UTF-16
szKey, pad to 4, value, pad to 4, children}`:

```
"VS_VERSION_INFO"  wType=0, value = VS_FIXEDFILEINFO (52 bytes):
    dwSignature 0xFEEF04BD, dwStrucVersion 0x00010000,
    dwFileVersionMS/LS, dwProductVersionMS/LS   <- version.file / version.product as 4 x u16
    dwFileFlagsMask 0x3F, dwFileFlags (VS_FF_DEBUG 0x1 | VS_FF_PRERELEASE 0x2)
    dwFileOS 0x40004 (VOS_NT_WINDOWS32), dwFileType (1 app / 2 dll / 3 driver), dwFileSubtype 0,
    dwFileDate 0
  "StringFileInfo" wType=1
    "<lang><cp>" e.g. "040904B0"   (language hex4 + codepage 04B0 = UTF-16)
      "CompanyName"      = company
      "FileDescription"  = description
      "FileVersion"      = version.file
      "InternalName"     = identifier
      "LegalCopyright"   = copyright
      "OriginalFilename" = <output file name>
      "ProductName"      = name
      "ProductVersion"   = version.product
  "VarFileInfo" wType=1
    "Translation" wType=0, value = u16 language, u16 0x04B0
```

Strings are UTF-16LE with terminating NUL, `wValueLength` counted in u16 units for string
blocks and bytes for the fixed block. Empty fields are skipped, not emitted empty.

#### macOS: Info.plist mapping and output shapes

Same `AppInfo` leaves, different container: a plist instead of a version block, a file
instead of an image. Two output shapes because a bare Mach-O can carry a plist but never an
icon.

| AppInfo field | Info.plist key | Note |
|---------------|----------------|------|
| `name` | `CFBundleName` | |
| `identifier` | `CFBundleIdentifier` | what `codesign` / LaunchServices key on |
| `version.product` | `CFBundleShortVersionString` | |
| `version.file` | `CFBundleVersion` | |
| `copyright` | `NSHumanReadableCopyright` | |
| `type` | `CFBundlePackageType` | always `APPL` for an executable |
| output name | `CFBundleExecutable` | |
| `icon` present | `CFBundleIconFile` = `<name>.icns` | bundle shape only |
| target | `LSMinimumSystemVersion` | from the deployment version the link already sets |
| `description`, `company`, `language` | (dropped) | no plist equivalent |

Strings are XML-escaped; nothing else is transformed.

**Bare executable, `-o app`.** The plist XML is emitted as a private constant global in
section `__TEXT,__info_plist`, added to `llvm.used` (dead-strip keeps it). This is Apple's
documented route for single-file tools: `codesign` reads the identifier from it,
LaunchServices reads the version, `otool -P` shows it. No linker flag, no `-sectcreate`,
works from the bundled `ld64.lld`. The icon is NOT representable here: Finder shows the
generic executable icon; `-v` prints a note, not an error.

**Bundle, `-o App.app`.** The compiler writes the layout Apple expects and links the
executable into it. `.app` is Apple's extension, not a new one; it is the one
bundle-emitting spelling.

```
App.app/Contents/Info.plist          from the same leaves
App.app/Contents/PkgInfo             "APPL????"
App.app/Contents/MacOS/App           the linked Mach-O, plist section included
App.app/Contents/Resources/App.icns  built from the PNG set, or the user .icns copied as-is
```

**ICNS layout**, built by the compiler with no external tool. File = `icns` magic + u32 total
length, then chunks `{4-char type, u32 length incl. header, body}`. Since 10.7 the body is the
raw PNG, so it is the same bytes the Windows RT_ICON entries got:

| PNG size | chunk | `@2x` chunk (same pixels, retina slot) |
|----------|-------|----------------------------------------|
| 16 | `icp4` | - |
| 32 | `icp5` | `ic11` (16@2x) |
| 64 | - | `ic12` (32@2x) |
| 128 | `ic07` | - |
| 256 | `ic08` | `ic13` (128@2x) |
| 512 | `ic09` | `ic14` (256@2x) |
| 1024 | `ic10` (512@2x) | - |

A size the user did not supply is simply absent; Finder scales the nearest. A supplied
`.icns` is copied verbatim; a supplied `.ico` on a macOS build is an error at the declaration.

**Runtime.** `Application.icon()` returns `NSApplication.applicationIconImage`, which Cocoa
resolves from the bundle plist; in a bare binary it is the generic icon, same as Finder.
`Application.name()` / `.version()` / `.identifier()` are compile-time constants on every
target and shape.

**Out of scope on macOS.** Code signing, entitlements, notarization (the plist section and
bundle layout are exactly what `codesign` consumes; the user runs it afterwards). `.lproj`
localization and nibs (`ui_native` / localization concerns).

#### Per-target summary

| Target | `name` / `version` / `identifier` / `copyright` | `icon` |
|--------|-----------------------------------------------|--------|
| Windows | RT_VERSION as above | RT_GROUP_ICON id 1 + RT_ICON 1..N |
| macOS, `-o app` | `__TEXT,__info_plist` section | not representable; `-v` note |
| macOS, `-o App.app` | `Contents/Info.plist` | `Contents/Resources/App.icns` |
| Linux | kept only as embedded data (readable via API below) | embedded PNG data only; `.desktop` + icon-theme emission deferred to a later `--bundle` plan |

#### Retrieval API (`core/application.cb`), all targets

- `Application.name()`, `.version()`, `.identifier()`, `.copyright()` - compile-time constants
  folded from the declaration, so an About box and the shell metadata can never disagree.
- `Application.icon()` - platform handle: Windows `LoadIcon(GetModuleHandle(nullptr), 1)`;
  macOS bundle icon via `NSApplication.applicationIconImage`; Linux decoded from the embedded
  PNG by the ui_native backend. `ui_native` sets the main-window icon from this automatically
  when an `application` declaration exists.
- Under `--run` the declaration compiles (constants still available) but emits no image
  metadata.

Inspection: `--dump-app-info -` prints the fields plus, per target, the VERSIONINFO string
block or the Info.plist XML - the counterpart of `--dump-manifest`, works with `--check` on
any host.

### 3.3 Out of scope, and where it goes instead

- **Dialog / menu / accelerator templates (RT_DIALOG etc.).** Not a resource problem; a
  `ui_native` problem. The motivation's real losses (`FONT` broadcast, DLU layout, tab order,
  default background paint) are things `ui_native` must provide on every backend, not things
  to recover by shipping Win32-only templates. File as `internal/issue/ui/` items against
  `ui_native`: font propagation to children, dialog-unit layout helper, default erase-bkgnd.
- **String tables / RT_STRING.** `embed string catalog = "strings/en.json";` plus `json.cb`
  already covers a runtime catalog, portably, with plural/format support RT_STRING never had.
  No language surface.
- **Cursors, bitmaps, message tables, typelibs.** `embed` covers the bytes; loading is the
  program's business.
- **Code signing, entitlements, notarization, `.desktop` generation.** Packaging; later
  `--bundle` stage.

## 4. Why not the alternatives

- `resource icon "app.ico";` (earlier candidate): a Windows-shaped surface; on macOS an
  `.ico` is useless and an icon is not even representable in a bare Mach-O. The
  `application` struct names the intent (app metadata), the compiler owns the container.
- CLI flags (`--icon`, `--app-version`): zero language change, but the About box cannot read
  the version the shell shows, values live in a build script, and the embed problem stays.
  Rejected 2026-09-02 in favour of the typed declaration.
- Emitting `.rsrc$01/.rsrc$02` from IR, or a user-supplied `.res`: the first re-implements
  what lld's `WindowsResource` library already does; the second needs rc.exe and gives
  macOS/Linux nothing.
- RT_RCDATA for `embed`: breaks `--run`, adds OS API for zero gain (see 3.1).
- A resource manifest file (`.qrc`/GResource style): rejected by the no-new-extensions
  constraint; the declaration IS the manifest, checked by the compiler with source locations.
- Fragment-merging `application` like `manifest`: two libraries disagreeing on the app name is
  never legitimate; one declaration, one owner.

## 5. Implementation order (after ratification)

1. `embed(...)` builtin (expression handler in `MainListener`, constant-fold to a global,
   context-typed `u8[N]` / `string`, dep-json entry). Gate: extend an existing `Test/test_*.cb` with an
   embedded text file from `Test/library/` compared byte-for-byte; runs under `test.sh` and
   `test.bat`, host-independent.
2. `application` decl + validation + `--dump-app-info` (host-independent), Windows
   `RT_VERSION` + `RT_GROUP_ICON` in the `.res` writer, PNG->ICO/ICNS conversion.
3. macOS `__TEXT,__info_plist` and `-o App.app` bundle writer.
4. `Application.*` API + `ui_native` auto window icon. Gate: `example/ui/` app with an
   `application` declaration, asserting non-null icon handle - `test_example.bat` on Windows,
   `test_example.sh` on macOS.
5. Linux `.desktop` emission - separate plan.
