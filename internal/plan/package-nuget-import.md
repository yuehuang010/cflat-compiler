# Plan: `import package-nuget` (NuGet package resolver)

Status: TABLED (future work, not started). Created: 2026-07-03.

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
