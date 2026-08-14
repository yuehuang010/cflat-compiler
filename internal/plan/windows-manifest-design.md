# Typed Windows manifests: compile-time field verification (DESIGN)

Status: PROPOSED DESIGN - not ratified, do NOT implement yet.
This doc absorbed `internal/issue/ui/win32-classic-common-controls-v5.md` (issue removed
2026-08-14); the comctl32 v6 dependency is the first fragment this design must carry, and
`internal/plan/ui-win32-native-polish.md` Phase 5 is the first consumer.

Background (from the absorbed issue): a cflat-linked exe contains NO application manifest
at all - no `assemblyIdentity` of any kind - so every cflat GUI app binds the CLASSIC (v5)
common controls. Consequences: `SetWindowTheme` is a no-op on control CLIENT areas (still
works on non-client parts); buttons, combo, trackbar thumb, and the size-grip glyph stay
light in dark theme; and a v5 `BUTTON` never sends `NM_CUSTOMDRAW`, so
`tryCustomDrawButton` in `core/ui_native/win32.cb` is dead code in a live window (its
self-test passes only by calling the draw function directly into a memory DC - fix per
polish-plan 5.4). Decision 2026-07-13: embed the v6 manifest rather than owner-draw on v5.

Scope: the Windows application-manifest system in general - SxS assembly dependencies
(comctl32 v6), `windowsSettings` (dpiAwareness, longPathAware, activeCodePage, heapType),
`supportedOS`, `requestedExecutionLevel`, and the WinRT/XAML side: `activatableClass`
entries for registration-free WinRT activation (what unpackaged WinUI apps need).
AppxManifest.xml (packaged apps) is out of scope for v1 but the mechanism generalizes.

Builds on the verified findings and constraints in `internal/plan/ui-win32-native-polish.md`
Phase 5.1a: no XML in `.cb`, no user-visible `.manifest` file, no manifest vocabulary in the
compiler, lld-link merges multiple `/manifestinput:` fragments without `mt.exe`, and
`CreateActCtxW` validates SxS semantics but NOT `windowsSettings` values.

## The core idea

The manifest is a typed value. Verification is layered so that each layer catches what the
others cannot, and the compiler never learns a single Windows vocabulary word:

| Layer | Where | Catches | Host |
|-------|-------|---------|------|
| 1. Type system | existing brace-init checking | unknown field names, wrong value types, out-of-vocabulary values (via enums) | all |
| 2. Structural transliteration | compiler (vocabulary-free) | nothing to catch - correct XML by construction (nesting, escaping, well-formedness) | all |
| 3. OS backstop | `CreateActCtxW` + per-field `QueryActCtxSettingsW` round-trip at compile time | bad publicKeyToken, nonexistent versions, unknown setting names, any field the loader would drop or 14001 on at launch | Windows host only |

Layer 1 covers exactly the hole layer 3 leaves (probed: `CreateActCtxW` ACCEPTS
`PerMonitorV9`), and layer 3 covers what types cannot know (does version 6.0.0.0 with this
token exist in WinSxS). Together, "the fields in the manifest are correct" is checked at
compile time on both axes: vocabulary AND OS semantics.

## Layer 1: vocabulary as core types

`core/manifest.cb` (new, stdlib data - not compiler C++) declares the schema as plain
structs and enums:

```cflat
enum DpiAwareness { Unaware, System, PerMonitor, PerMonitorV2 }

struct AssemblyIdentity
{
    string type = default;                  // attribute
    string name = default;
    string version = default;
    string processorArchitecture = default;
    string publicKeyToken = default;
    string language = default;
}

struct WindowsSettings
{
    xtext<DpiAwareness> dpiAwareness;       // element with text content
    xtext<bool> longPathAware;
    string xmlns = "http://schemas.microsoft.com/SMI/2016/WindowsSettings";
}

struct ManifestFragment                     // serializes as the <assembly> root
{
    string xmlns = "urn:schemas-microsoft-com:asm.v1";
    string manifestVersion = "1.0";
    list<AssemblyDependency> dependency = default;
    ApplicationSettings application = default;
    list<ActivatableClass> activatableClass = default;   // regfree WinRT / XAML
}
```

Field-name verification is FREE: brace-init of a known struct type already rejects unknown
fields, so `dpiAwarness = ...` is a compile error today with no new checking code. Closed
value sets become enums, so `PerMonitorV9` is unrepresentable - the exact class of defect
the OS accepts silently. Magic constants live in core as named values plus helper
constructors (`Manifest.commonControlsV6()`), so nobody ever hand-types `6595b64144ccf1df`.

### Where the vocabulary comes from (discovery)

There is NO machine-readable schema to extract from (5.1a finding: no SxS `.xsd` in the
SDK, no header structs, no WinMD types) - so the vocabulary is CURATED, not generated.
Sources, in order of authority:

1. **Microsoft Learn "Application manifests" reference** - the authoritative enumeration
   of every element and attribute (assemblyIdentity, compatibility/supportedOS, trustInfo,
   the full `windowsSettings` list, dependency, file/comClass for regfree COM,
   activatableClass, msix). The schema is small, closed, and slow-moving (new entries
   roughly once per Windows release), which is what makes hand-curation tenable at all.
   MEASURED 2026-08-14 via `utilities/windows-manifest/scrape-manifest-schema.ps1` (the
   inventory it produced is checked in next to it as `manifest-schema.json`): 28 elements,
   19 attributes concentrated in 8 elements, 26 enumerable values, ~15 simple
   windowsSettings-style leaves - so `core/manifest.cb` is roughly 60-80 declarations.
2. **Empirical corpus for coverage checking** - `C:\Windows\WinSxS\Manifests` (thousands
   of real manifests) plus `RT_MANIFEST` resources extracted from system exes. Used to
   confirm the curated set covers what real software actually declares, never as a source
   of truth for semantics.
3. **Machine-readable sources where they exist, for VALUES not shape**:
   - `activatableClass` class names: cross-checkable against the `.winmd` metadata cflat
     already parses (WinmdSignature) - a declared class the winmd does not contain can be
     a compile error. Optional, not v1.
   - `processorArchitecture`: derived from the compile target, never typed by hand.
   - `supportedOS` GUIDs: a documented closed list - becomes an enum with named members,
     the GUIDs are the serialized values.
4. **CreateActCtxW probes at authoring time** - each curated element gets a scratch probe
   confirming the OS accepts what the transliteration emits, the same way the 2026-07-13
   probes validated the defect table. Layer 3 then catches curation drift permanently.

Curation drift is the accepted cost of "no vocabulary in the compiler": when Windows adds
a setting, the fix is a field added to `core/manifest.cb` - a stdlib data edit with no
compiler change, which is exactly the point of the layering.

LSP typing (hover, go-to-def) comes free from `LspSymbolIndex` indexing struct fields.
Optional follow-on, valuable language-wide and not manifest-specific: completion inside a
brace-init of a known struct type (`LspServer.cpp` already tracks the enclosing type per
open brace; it only lacks the trigger - see 5.1a GAP note).

## Layer 2: vocabulary-free transliteration

The compiler knows XML SHAPE only. Mapping rules, applied to the folded initializer:

- struct -> element; the ELEMENT NAME is the FIELD name (so `list<AssemblyDependency>
  dependency` emits repeated `<dependency>` elements; the type name never appears in XML).
- scalar field -> attribute (`name="..."`); values XML-escaped (`&<>"'`).
- `list<T>` -> repeated child elements.
- `xtext<T>` (new generic marker type in core, like `attr<T>` from the brainstorm but only
  ONE marker is needed): element whose TEXT CONTENT is the value -
  `<dpiAwareness>PerMonitorV2</dpiAwareness>`. Enum values serialize as their name; bools
  as `true`/`false`.
- a field literally named `xmlns` -> the `xmlns` attribute (reserved name, not vocabulary).
- fields left at `default` (empty string / empty list / absent) are omitted entirely.

That is the whole compiler-side XML knowledge: five structural rules and one marker type.
All names in the SxS schema are valid cflat identifiers (checked: assemblyIdentity,
windowsSettings, supportedOS attributes, activatableClass) so no rename escape is needed
in v1; if a future schema needs one, add it then (YAGNI).

## Layer 3: OS backstop at compile time

On a Windows host, after folding+serializing each fragment and the merged document (below),
run `CreateActCtxW` on the XML (written to a compiler-internal temp; the user never sees a
`.manifest` file). Failure maps to `LogErrorContext` at the manifest declaration's source
location. This runs in the normal analysis path, so `--check` and the LSP get it too - a
manifest defect is a live squiggle, not a launch-time 14001 on an end user's machine.
Non-Windows hosts cross-compiling to COFF degrade to layers 1-2 (still full vocabulary
checking; only the OS round-trip below is lost).

### Field-loaded verification (probed 2026-08-14, scratch/manifest_probe/probe2.ps1)

"Did the OS actually LOAD this field" is directly verifiable at compile time, no process
launch, via the activation context the loader itself builds:

| probe | result |
|-------|--------|
| unknown setting NAME (`notARealSetting`, 2016 ns) | `CreateActCtxW` REJECTS - gle 14001 |
| `QueryActCtxSettingsW(h, 2016ns, "dpiAwareness")` | returns `'PerMonitorV2'` verbatim |
| `QueryActCtxSettingsW` on a never-declared name | fails gle 14007 (key not found) |
| bogus VALUE (`PerMonitorV9`) | accepted, stored, round-trips verbatim |
| null settings namespace for a 2016-ns setting | gle 87 - always pass the explicit ns |

This REVISES the 5.1a defect table: the OS validates windowsSettings field NAMES (unknown
names 14001 at context creation), it only ignores VALUES - which is exactly what layer-1
enums cover. So the backstop asserts, per windowsSettings field the fragments declare:
`QueryActCtxSettingsW` returns the exact value we serialized. A typo'd name, a wrong
`xmlns`, or a field lost in merging is a hard compile error; nothing can be silently
dropped. For `dependency` entries, `QueryActCtxW` with
`AssemblyDetailedInformationInActivationContext` enumerates the assemblies that actually
resolved into the context - same shape of check (probe during implementation).

The last gap - "the value is stored, but does the OS component HONOR it at runtime" - is
not a compile-time question. It is covered at test time by runtime getters in the gallery
selftest: `GetProcessDpiAwarenessContext` (dpiAwareness), `GetACP` == 65001
(activeCodePage utf-8), `RtlAreLongPathsEnabled` (longPathAware), comctl32
`DllGetVersion` >= 6 (the v6 dependency).

Implementation probe needed (scratch, before coding): does `CreateActCtxW` accept a
fragment with no `assemblyIdentity` of its own? If not, validate only the merged document
and rely on layers 1-2 per fragment. (The probes above ran with an identity present.)

## Declaration, trigger, and composition

- Trigger: a `manifest` SOFT keyword on a global declaration - text-match in BOTH
  `ParseDeclarationSpecifiers` copies (like `move`), NOT an ANTLR lexer token. One keyword,
  zero vocabulary:

  ```cflat
  manifest ManifestFragment comctlV6 = Manifest.commonControlsV6();
  // or fully spelled out with brace-init - both fold the same way
  ```

- Const-fold straight off the parse tree: the initializer may contain literals, enum
  references, nested braces, list literals, and calls to core helper functions that are
  themselves brace-returning (fold by inlining the returned literal). Anything else is a
  `LogError` ("manifest initializer must be a compile-time literal"). This deliberately
  does NOT depend on general const-eval or const globals (known unimplemented gap).

- Ownership shape: fragments compose, so a library owns its own OS requirement -
  `core/ui_native/win32.cb` declares the comctl32 v6 fragment, `core/ui_native/winui.cb`
  declares PerMonitorV2 (replacing its current runtime opt-in at winui.cb:272) and its
  `activatableClass` entries. `import "ui_native.cb"` drags the manifest in exactly the way
  the existing `lib` and `pri` clauses drag in link inputs. USER `.cb` files may also
  declare fragments in v1 - this is required, not optional, because `activatableClass`
  entries for unpackaged WinUI are app-specific and cannot live in core.

- Emission: each fragment -> one temp XML -> its own `/manifestinput:` in `EmitExecutable`'s
  `linkArgStrs`, plus `/manifest:embed`. lld merges (verified 2026-07-13). COFF path only;
  ELF/MachO untouched. Unconditional - no opt-out flag unless a real need appears.

- Conflict rule (vocabulary-free): byte-identical fragments dedupe silently. For the
  compiler-side merged document used in layer 3: two text-elements at the same element path
  with the SAME identifying attributes but DIFFERENT text or attributes -> `LogError`
  naming both source locations (e.g. two different Common-Controls versions). This is a
  structural rule; it never names a Windows element.

## Cache round-trip (load-bearing)

Manifest declarations will live in core `.cb` files, which `--init` serializes to bitcode
plus hand-written compiler state. The folded fragments MUST be added to the
`LLVMBackend.cpp` cache round-trip in the same change, or a warm cache silently drops every
core-declared manifest and ships unmanifested exes again - the exact failure mode of the
original issue, now intermittent. Serialize the fragment as (source location, serialized
XML string): the XML is the fold's output, so nothing structural needs re-folding on load.

## Verification

- `Test/errors/`: unknown field name, wrong enum value, non-literal initializer, conflicting
  duplicate fragment - all via `expect_error` (extend existing files where possible per the
  no-new-test-files rule; new `err_*.cb` files are the sanctioned exception).
- Positive: build a GUI example and assert `Microsoft.Windows.Common-Controls` appears in
  the emitted exe bytes (the check from the issue file); visual pass over GUI examples in
  both themes per Phase 5 of the polish plan.
- LSP: `test_lsp.bat` after any `LspServer.cpp` change (brace-init completion follow-on).

## Open decisions for ratification

1. Marker type spelling: `xtext<T>` as the single XML primitive (this doc), vs the
   brainstorm's `attr<T>`/`elem<T>` pair. Default mapping makes the pair redundant; one
   marker is the smaller compiler surface.
2. Element name = field name (this doc) vs struct type name. Field name wins because it
   makes `list<T> dependency` natural and keeps type names out of the XML contract.
3. Helper-function folding (inline a brace-returning core function) - accept the small
   fold complexity, or v1 ships brace-init-only and `Manifest.commonControlsV6()` becomes
   a named const fragment instead.
4. Conflict rule strictness: LogError on same-path/different-value (this doc), vs defer
   entirely to lld merge + CreateActCtxW of the merged doc.
