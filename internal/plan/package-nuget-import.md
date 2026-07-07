# Plan: `import package-nuget` (NuGet package resolver)

Status: v1.1 IMPLEMENTED 2026-07-06 (grouped import form added on top of v1; grammar +
4 dispatch sites + CompileNugetImport(vector) + CompileCHeaderGroup reuse; test.bat/
test_lsp green; live smokes: single-entry group binds WebView2.h, two-entry group resolves
both WebView2 headers under the package include dirs into one TU, strict-rejection of a
system header in a group). v1 IMPLEMENTED 2026-07-06 (grammar + dispatch + NugetResolver.h +
MsbuildLite.h; test.bat/test_lsp green; live smokes: WebView2 header+link+run,
WinAppSDK.WinUI winmd unpinned, dotnet-restore AND curl+tar acquisition both exercised).
Created: 2026-07-03.
Winui example migration DONE 2026-07-06: winui_host.cb/winui_app_demo.cb now pull
Microsoft.UI.Xaml.winmd (WinUI package), Microsoft.UI.winmd (InteractiveExperiences
package), and Microsoft.Windows.ApplicationModel.DynamicDependency.winmd (Foundation
package) via three pinned `import package-nuget` lines instead of in-tree copies; the
in-tree example\ui\winui\winmd\ dir (and its two .winmd files) is deleted. The
Foundation import is consumed purely for its side effect of adding
Microsoft.WindowsAppRuntime.Bootstrap.lib to the link and Bootstrap.dll to the deploy
set for the hand-written MddBootstrapInitialize2/MddBootstrapShutdown externs -
MddBootstrap.h itself still cannot be imported directly (does not compile standalone; it
needs the system windows.h as a prerequisite, which the strict package-only grouped-import
rule below deliberately excludes - so the winui examples keep the hand-written externs +
the Foundation DynamicDependency.winmd import for the bootstrap lib/dll BY DESIGN).
example.bat no
longer hardcodes a WINUISDK package path or --c-lib/copy step in --worker-winui; it now
gates the two winui self-tests on a WINUINUGET flag that checks the NuGet cache (via
NUGET_PACKAGES override or the default packages folder) for all three pinned package
versions and SKIPs (not fails) when absent. The resolver was also extended to probe
versioned `metadata\<ver>\` subdirectories (needed to find Microsoft.UI.winmd inside the
InteractiveExperiences package layout).
Remaining: test.sh re-verify.

## Grouped import (v1.1)

`import package-nuget` accepts a brace group of headers compiled as ONE translation
unit, for packages whose headers depend on each other:

```cflat
import package-nuget { "a.h", "b.h" } from "Some.Package/1.2.3";
```

Decided semantics (do not soften):
- STRICT package-only: every entry in a group must resolve under the resolved
  package's include dirs. An entry not found there (e.g. "windows.h") is a compile
  error - system headers may NOT ride in a package-nuget group. (Transitive
  `#include <windows.h>` from inside a package header is fine; only the cflat group
  ENTRIES are constrained.)
- A multi-entry group may contain ONLY .h/.hpp/.hh entries. A .winmd inside a
  multi-entry group is an error ("group a .winmd import on its own line"). This shape
  check runs BEFORE package resolution, so it errors with no network/cache access.
- A single-entry group `{ "x" }` behaves EXACTLY like the bare `"x"` form (headers
  route through BindCanonicalCHeader; .winmd through the WinRT pipeline - both fine).
- All entries in a multi-entry group share one TU and one disk-cache entry via
  CompileCHeaderGroup (same machinery as the plain `import { "a.h", "b.h" };` group);
  the resolved canonical header paths all live under the package include dirs.
- `define` clauses apply to the whole group TU.
- LSP mode keeps the existing degrade-to-silent-skip behavior (unresolved package, or
  a header not found under the include dirs) instead of painting errors.

Implementation: the grammar switches the package-nuget alternative from a direct
StringLiteral to `importGroup`; the four import-dispatch sites (ProcessImports, nested
imports, Analyze, and the if-const branch in MainListener) dispatch on the
`package-nuget` keyword BEFORE any importGroup-based routing (a multi-entry nuget group
is one package TU, not several plain imports); `CompileNugetImport` takes a
`std::vector<std::string>` and branches single-entry (unchanged) vs multi-entry
(strict resolve + CompileCHeaderGroup). Error test:
`Test/errors/err_nuget_group_winmd_in_group.cb`. No hermetic test exists for the
"header not in package include dirs" error (it needs a resolved package in the local
cache); it is proven live on this box instead.

## Motivation

The P6 WinUI 3 track (see ui-native-framework.md) needs build-time artifacts that
only ship in a NuGet package: `Microsoft.WindowsAppSDK` is the sole sanctioned
source of `Microsoft.UI.Xaml.winmd` / `Microsoft.UI.winmd`, `MddBootstrap.h`, and
`Microsoft.WindowsAppRuntime.Bootstrap.{lib,dll}`. There is no OS metadata store
for them (unlike `%SystemRoot%\System32\WinMetadata`), the runtime MSIX folder is
version-named and ACL-restricted, and the NuGet cache only exists if some other
project happened to restore the package. On a fresh machine cflat has no way to
obtain them today - the winui examples currently carry copied winmds in-tree.

General value beyond WinUI: NuGet is the distribution channel for many native
Windows SDKs (WinAppSDK, WebView2, DirectX headers/agility SDK, WIL). A resolver
gives cflat a one-line, reproducible way to consume them, exactly like
`package-vcpkg` did for vcpkg ports.

## Model: mimic `package-vcpkg`

Existing precedent (grammar CFlat.g4 ~line 687, dispatch LLVMBackend.cpp
`IsPackageVcpkgImport`/`CompileVcpkgImport`, resolver `VcpkgResolver.h`):

```cflat
import package-vcpkg "SDL3/SDL.h" from "sdl3" define "SDL_MAIN_HANDLED";
```

Proposed mirror:

```cflat
// header binding from a native NuGet package
import package-nuget "MddBootstrap.h" from "Microsoft.WindowsAppSDK.Foundation/1.8.260222000";

// winmd import from a NuGet package
import package-nuget "Microsoft.UI.Xaml.winmd" from "Microsoft.WindowsAppSDK/1.8.250907003";
```

- `from` = package id, optionally `/version` (pin). Unpinned = latest stable
  already in the cache; resolver errors with the pin suggestion if absent
  (keep builds reproducible - do NOT silently float to nuget.org latest).
- The first path segment routes by extension exactly like normal imports:
  `.h/.hpp` -> C header binding pipeline, `.winmd` -> WinRT metadata pipeline.
- `define` clauses forward to the header pipeline as with package-vcpkg.

## NugetResolver (new, header-only, mirrors VcpkgResolver)

Resolution order per package:
1. Global packages folder: `%USERPROFILE%\.nuget\packages\<id>\<ver>\`
   (respect NUGET_PACKAGES env override). Already-restored packages resolve
   with zero tooling installed.
2. Acquire on miss: a .nupkg is a plain zip served by the NuGet v3
   flatcontainer API
   (`https://api.nuget.org/v3-flatcontainer/<id>/<ver>/<id>.<ver>.nupkg`).
   Prefer shelling to `nuget.exe`/`dotnet` when discovered on PATH (mirrors
   vcpkg.exe discovery); direct HTTPS download + unzip into the cache layout
   is the fallback. `--nuget-no-install` mirrors `--vcpkg-no-install`
   (and LSP mode never downloads).
3. Expose a Resolution mirroring VcpkgResolver::Resolution:
   - includeDir(s): `include\`, `build\native\include\`
   - winmd dirs: `lib\uap10.0\`, `metadata\`, `lib\net*\...` (probe order TBD)
   - libs: `lib\native\<arch>\*.lib`, `runtimes\win-<arch>\native\*.lib`
   - dlls to deploy next to the exe: `runtimes\win-<arch>\native\*.dll`
     (this is how Microsoft.WindowsAppRuntime.Bootstrap.dll gets placed)
   - arch from --platform like the vcpkg triplet derivation.

CLI symmetry: `--nuget-exe`, `--nuget-packages-dir`, `--nuget-no-install`.

## MSBuild .props/.targets lightweight evaluation (decided 2026-07-06)

The package's own `build/native/<PackageId>.targets` (+ `.props`) is the
authoritative statement of include dirs, libs, and deploy DLLs - directory
probing is only a heuristic. Decision: parse the RELEVANT SUBSET of MSBuild
XML instead of probing first.

Scope for v1 (lightweight, defer full MSBuild semantics):
- `<PropertyGroup>` / property definitions, with `$(Prop)` expansion.
  Pre-seed well-known properties: `MSBuildThisFileDirectory` (dir of the
  .targets file), `Platform` / `PlatformTarget` (from --platform: x64/x86/
  arm64), `Configuration` (Release).
- `<ItemGroup>` items + metadata. Recognized item types feed the Resolution:
  include dirs (AdditionalIncludeDirectories appends inside
  `<ClCompile>`-style definition groups), `.lib` items
  (AdditionalDependencies / Link metadata), and copy-to-output items
  (`<None>`/`<Content>`/custom items with CopyToOutputDirectory) for DLLs.
- `Condition` attributes: a small evaluator covering `'$(x)'=='y'` / `!=`,
  `Exists(...)`, `And` / `Or`, parentheses. Unknown constructs evaluate as
  false, with a -v diagnostic.
- NOT in scope: `<Target>` elements (skipped entirely), `<Import>` chains
  beyond same-directory relative imports, property functions
  (`$([MSBuild]::...)`), wildcards/transforms in item specs.
- Fallback: if the .targets is missing or evaluation yields nothing usable,
  fall back to the directory-convention probe order above.

Verified layouts on this box (2026-07-06): WebView2 = everything under
`build/native/{include,x64,arm64,x86}`; WinAppSDK.Foundation = top-level
`include/` + `metadata/*.winmd` + `lib/native/<arch>/*.lib` +
`runtimes/win-<arch>/native/*.dll`; WinAppSDK.WinUI = `metadata/` only (no
libs); WIL = header-only `include/wil`. Ignore `runtimes-framework/`
(framework-package payload, wrong for self-contained apps).

## Acquisition detail: NuGet v3 flatcontainer protocol

Locate-and-download flow for step 2 above (all ids/versions LOWERCASED,
versions SemVer2-normalized - no leading zeros, no build metadata):

1. Service index: GET `https://api.nuget.org/v3/index.json`, find the
   resource of type `PackageBaseAddress/3.0.0` -> flatcontainer base URL.
   (May hardcode the current `https://api.nuget.org/v3-flatcontainer/` as
   fallback, but the index query is the documented contract.)
2. Version list (validation / pin suggestion on miss): GET
   `<base>/<id>/index.json` -> `{"versions": [...]}`.
3. Package: GET `<base>/<id>/<ver>/<id>.<ver>.nupkg` - a plain zip.
4. Install into the global packages folder layout so real NuGet tooling
   recognizes it: extracted tree + the .nupkg itself +
   `<id>.<ver>.nupkg.sha512` + `.nupkg.metadata` (JSON w/ contentHash).
   If mimicking that layout proves fiddly, fall back to a private
   `~/.cflat/nuget/` cache dir (resolution order then: NUGET_PACKAGES ->
   global folder -> private cache).

Transport options, in preference order:
- Synthesized temp project + `dotnet restore` when dotnet is on PATH -
  produces a byte-correct global-cache install, zero protocol code.
- `nuget.exe install` if found (legacy layout differs - normalize after).
- Direct download via shelling `curl.exe` + extract via `tar.exe` (both
  ship in Windows 10+ System32; bsdtar reads zip) - zero new dependencies,
  mirrors how cflat already shells clang-cl/lld-link.
nuget.org is anonymous HTTPS; private/authenticated feeds are out of scope
for v1.

## Open questions (decide at implementation time)

1. Dependency closure: native NuGet packages sometimes chain (WindowsAppSDK
   1.8 split into Foundation/... metapackages). v1 stance: NO transitive
   resolution - each needed package is its own import line (matches the
   package-vcpkg "manifest declares it" spirit; avoids reimplementing NuGet).
2. Version floating vs pinning: recommend always pinning in examples; decide
   whether unpinned is an error or a cache-latest convenience.
3. winmd probe order inside a package (uap10.0 vs metadata folders) - survey
   WinAppSDK, WebView2, Win2D layouts first.
4. Whether `lib` deployment should copy DLLs automatically (vcpkg path already
   returns dlls; follow whatever CompileVcpkgImport does with them).
5. Signature/hash verification of direct downloads (nuget.org serves over
   HTTPS; decide if that is enough for v1).

## First consumer / acceptance

- Replace the copied winmds in `example/ui/winui/winmd/` with
  `import package-nuget` lines; delete the in-tree binaries.
- `example.bat` winui gate: detect resolvability (cache hit or tooling
  present) and SKIP, not fail, when the package cannot be resolved offline.
- Gate: winui demos green via the resolver on this box; test.bat/test_lsp/
  example.bat all green; fresh-machine story documented in doc/CLI.md.
