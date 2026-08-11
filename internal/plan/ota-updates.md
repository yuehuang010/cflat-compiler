# Plan: OTA updates for compiler and signed `.cb` application bundles

Status: DESIGN + LOCAL LAUNCHER PROTOTYPE.
Created: 2026-08-11.

## Goal

Support production-grade over-the-air updates for cflat-managed deployments where:

- the installed compiler can update itself from a configured update site;
- users can land signed `.cb` application bundles from a safe web portal;
- cflat verifies authenticity before compiling or activating any downloaded content;
- update failures preserve the last known-good compiler and application release;
- launch failures roll back automatically.

The portal implementation is out of scope. This plan defines the update contract that the
portal, release tooling, launcher, and compiler must share.

## Non-goals

- Do not treat native CFlat code as sandboxed. Signature verification proves publisher
  authenticity and file integrity; it does not make arbitrary native code safe to run.
- Do not compile or execute unsigned downloaded `.cb` input in production mode.
- Do not let HTTPS alone be the trust boundary. TLS is transport protection only.
- Do not replace a running `cflat.exe` in place on Windows.

## Current state

cflat can compile local `.cb` files and has a version-sensitive `.cflat` cache populated by
`--init` or `--init-local`. The cache stores compiler-derived artifacts and must remain tied
to the compiler/runtime that produced it.

There is no file signature check today. Any `.cb` file that reaches disk can be parsed by the
compiler if the caller passes it as input. OTA support therefore needs a new authenticity gate
that runs before package extraction, before parsing, and before cache reuse.

## Architecture

Split OTA into three cooperating pieces:

- `cflat-launcher`: stable executable users invoke. It checks for compiler updates, starts the
  selected compiler, supervises readiness, and owns rollback.
- `cflat-updater`: library or helper used by the launcher and by explicit CLI commands. It
  fetches manifests, verifies signatures and hashes, stages releases, and atomically activates
  them.
- `cflat`: the compiler. It verifies signed `.cb` bundles before compiling them, writes
  version-scoped cache entries, and exposes read-only verification/status commands.

The launcher should be smaller and more stable than the compiler. Updating the launcher itself
is allowed but should be rare and should use the same signed-release mechanism with a
two-process swap on Windows.

## Release layout

Install into immutable slots:

```text
<install-root>/
  current -> releases/compiler/1.8.4+72/
  previous -> releases/compiler/1.8.3+70/
  releases/
    compiler/<version>/
      cflat.exe
      core/
      runtime.cb
      manifest.cflat
      manifest.cflat.sig
    app/<app-id>/<version>/
      src/
      app.manifest.cflat
      app.manifest.cflat.sig
      build/
      run/
  update-state.json
  update-lock
```

On Windows, `current` can be a small text pointer file instead of a symlink if symlink
privileges are unavailable. Activation is pointer replacement, not in-place mutation of an
active release directory.

## Prototype first: local launcher

Build the launcher before the network updater or portal integration. The first prototype uses
a local filesystem feed that has the same shape as the future web feed, so the launch,
staging, activation, verification, and rollback logic can become real without waiting on the
website.

Prototype goals:

- create a small `cflat-launcher` executable;
- keep the existing compiler binary unchanged while the launcher proves itself;
- run the currently active compiler slot with forwarded CLI arguments;
- install or activate compiler releases from a local directory;
- preserve a previous known-good compiler slot;
- verify before activation and verify again before run;
- write deterministic `update-state.json` state;
- support rollback and status inspection.

Prototype non-goals:

- no network fetcher;
- no background update schedule;
- no launcher self-update;
- no portal account/authentication flow;
- no signed `.cb` app bundle compiler integration until the compiler-release launcher path is
  proven.

Local feed shape:

```text
<local-feed>/
  compiler/
    stable/
      channel.cflat
      channel.cflat.sig
      releases/
        1.8.4+72/
          cflat-windows-x64-1.8.4+72.zip
          manifest.cflat
          manifest.cflat.sig
```

The local `channel.cflat` can be a small signed index:

```text
cflat-channel-v1
scope=compiler-channel
product=cflat
channel=stable
latest_sequence=72
latest_version=1.8.4+72
manifest_path=releases/1.8.4+72/manifest.cflat
signature_path=releases/1.8.4+72/manifest.cflat.sig
artifact_path=releases/1.8.4+72/cflat-windows-x64-1.8.4+72.zip
end
```

For the very first launcher prototype, the channel index can be optional: a CLI command may
point directly at a release manifest and artifact. Keep the verification API the same so the
prototype does not bake in a special unsigned path.

Launcher command surface for the prototype:

- `cflat-launcher --status`: print install root, active compiler, previous compiler, failed
  releases, and last update error.
- `cflat-launcher --run -- <cflat args>`: run the active compiler after verifying the active
  slot.
- `cflat-launcher --update-from <local-feed>`: read the local feed, verify the newest compiler
  release, stage it, initialize its local cache, activate it, and run a smoke check.
- `cflat-launcher --install-release <manifest> <signature> <artifact>`: install one local
  release directly without a channel file. This is useful for tests and manual development.
- `cflat-launcher --rollback`: switch active compiler back to the previous known-good slot.
- `cflat-launcher --no-update -- <cflat args>`: run active compiler and skip feed checks.

The eventual user-facing `cflat.exe` can become the launcher after this proves out. Until
then, keep the prototype binary named `cflat-launcher` to avoid disrupting normal compiler
development.

Launcher startup path:

1. Resolve install root. Default to the directory containing the launcher for portable installs.
2. Acquire `update-lock`. If another launcher owns it, skip update work and run the active
   compiler.
3. Load `update-state.json`. If it is missing, create an initial state that points at the
   seeded compiler slot.
4. Verify the active compiler slot before running it: manifest signature, file hashes, expected
   executable path, and local cache metadata.
5. If verification fails, try the previous known-good slot. If that also fails, stop with a
   clear error.
6. If an update command was requested, verify and stage the new release before changing any
   active pointer.
7. Activate by replacing the `current` pointer and recording the old active slot as
   `previous`.
8. Run `cflat --init-local` inside the new slot.
9. Run a smoke check such as `cflat --version` or `cflat --check <minimal signed smoke file>`.
10. Mark the release known-good only after the smoke check succeeds.
11. Forward normal CLI arguments to the active compiler and return its exit code.

`update-state.json` prototype schema:

```json
{
  "schema": 1,
  "install_root": "C:/cflat",
  "channel": "stable",
  "active_compiler": {
    "version": "1.8.4+72",
    "sequence": 72,
    "path": "releases/compiler/1.8.4+72",
    "manifest_sha256": "..."
  },
  "previous_compiler": {
    "version": "1.8.3+70",
    "sequence": 70,
    "path": "releases/compiler/1.8.3+70",
    "manifest_sha256": "..."
  },
  "highest_accepted": {
    "compiler-release:cflat:stable": 72
  },
  "failed_releases": [
    {
      "version": "1.8.5+73",
      "sequence": 73,
      "reason": "smoke check failed",
      "time_utc": "2026-08-11T00:00:00Z"
    }
  ],
  "last_error": null
}
```

State writes must be atomic: write a new file next to `update-state.json`, flush it, then
rename over the old state. The launcher must tolerate a missing or corrupt state file by
falling back to a verified slot scan instead of deleting releases.

Minimal first implementation:

1. Add `cflat-launcher` target with path utilities, lock file, state load/save, and process
   spawn/exit-code forwarding.
2. Support a seeded install root with one compiler slot and `--status`.
3. Add active-slot verification using the manifest/file hash verifier. Signature verification
   can initially be behind the same API even if the first local development fixture uses a test
   key.
4. Add `--run -- <args>` and make it return the real compiler exit code.
5. Add direct local install with `--install-release <manifest> <signature> <artifact>`.
6. Add pointer activation, `--init-local`, smoke check, quarantine, and rollback.
7. Only after this path works, add local channel index support.

Prototype implementation status:

- Implemented `cflat-launcher` as a separate C++ target in `launcher/Launcher.cpp` and
  `launcher/Sha256.cpp`. It does not link LLVM or depend on the compiler it launches.
- The launcher resolves a portable install root next to itself by default, with `--root` for
  isolated test installs. It supports `--status`, `--run --`, `--no-update --`,
  `--install-release`, `--update-from`, and `--rollback`.
- The first local artifact input is an unpacked release directory. ZIP extraction and network
  transport remain later work; the feed/channel metadata paths are already shaped like the
  eventual web feed.
- SHA-256 is the explicit prototype stand-in for publisher signatures. A detached
  `manifest.cflat.sig` is still required and must contain matching `signed_sha256` and
  `signature` values for the exact manifest bytes. This proves file integrity only; it does
  not authenticate a publisher and must be replaced by Ed25519 before production use.
- Compiler slots are verified before activation and reverified immediately before every run.
  `--init-local` and `--version` smoke checks run before activation, and failed staging is
  quarantined. The active pointer and `update-state.json` are atomically replaced.
- A sibling `cflat.exe` without a manifest is accepted only as the seeded local-development
  slot, so the prototype can launch the current build before its first managed release is
  installed. Managed slots always require the manifest and SHA-256 stand-in metadata.
- Added a CTest-backed integration harness with a deterministic fake compiler. It covers the
  SHA-256 known vector, seeded/managed launch, argument and exit-code forwarding, no-update
  mode, initialization, activation, replay rejection, rollback, corrupt-state recovery,
  tamper fallback, invalid metadata, protected active releases, and local channel updates.
- Added `test_launcher.bat`, following the existing `test.bat` contract: it selects Debug or
  Release, assumes the CMake targets are already built, validates the three required binaries,
  captures the harness log under `out-launcher/results`, prints named case results, reports
  elapsed time, and returns the harness exit code.

## Update flow

1. Acquire an exclusive update lock.
2. Download the channel index from the configured update URL.
3. Verify the signed channel index and choose the newest allowed sequence.
4. Download the release manifest and detached signature.
5. Verify the manifest signature with a pinned trusted key for the required scope.
6. Download the artifact into a staging directory.
7. Verify artifact size and SHA-256 while streaming.
8. Extract into a new immutable release slot using safe path rules.
9. Verify every extracted file listed in the manifest, and reject unlisted files unless the
   manifest explicitly allows them for a known directory such as generated build output.
10. For compiler releases, run `cflat --init-local` inside the new slot.
11. For `.cb` application bundles, compile into the new app release slot with the selected
   compiler and version-scoped cache.
12. Atomically activate the new pointer.
13. Launch the new release and wait for a `READY` handshake or successful smoke command.
14. Persist success only after readiness. Otherwise revert the pointer to the previous
   release and mark the failed release as quarantined.

Network failure, missing updates, malformed metadata, hash mismatch, signature failure, compile
failure, and launch failure all leave the current release active.

## Signed metadata model

Use signed manifests, not signatures over raw files alone. A manifest describes what the
artifact is, who may consume it, and the exact bytes allowed on disk. The signature is over
canonical manifest bytes.

There are two artifact scopes:

- `compiler-release`: signed by the compiler release key.
- `app-bundle`: signed by the portal application signing key.

The verifier must reject a valid signature from the wrong scope. A portal key cannot sign a
compiler release, and a compiler release key cannot sign user `.cb` app bundles unless it is
explicitly configured for that scope.

## Signature scheme

Use Ed25519 with SHA-256 for artifact and file hashes.

Why Ed25519:

- small public keys and signatures;
- deterministic signatures;
- no ASN.1 or padding modes to configure incorrectly;
- fast verification;
- available in mature libraries.

Implementation options, in preferred order:

1. Use OpenSSL EVP Ed25519 if OpenSSL is already an accepted dependency in the build.
2. Otherwise add a small `SignatureVerifier` abstraction and wire it to platform crypto where
   practical.
3. If neither is acceptable, explicitly approve a new crypto dependency before editing
   `vcpkg.json`; do not invent an in-house Ed25519 implementation.

Production trust anchors are compiled into the launcher/compiler as public keys. Private keys
exist only in release CI or the portal signing service.

## Canonical manifest format

Avoid JSON canonicalization in the first implementation. Use a strict UTF-8, line-oriented
manifest format whose exact signing bytes are the file bytes of `manifest.cflat`.

Rules:

- bytes are UTF-8 with LF newlines only;
- no BOM;
- keys are ASCII lowercase with `_`;
- fields appear in the exact order specified by the schema;
- repeated `file` entries are sorted by canonical path;
- paths use `/`, are relative, and must not contain empty segments, `.`, `..`, `:`, backslash,
  drive roots, leading `/`, or NUL;
- duplicate keys are invalid except repeated `file`;
- unknown keys are invalid for `schema=1`.

Example compiler manifest:

```text
cflat-manifest-v1
scope=compiler-release
product=cflat
channel=stable
sequence=72
version=1.8.4+72
platform=windows-x64
min_launcher_sequence=12
created_utc=2026-08-11T00:00:00Z
artifact_name=cflat-windows-x64-1.8.4+72.zip
artifact_size=18432000
artifact_sha256=0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef
file=cflat.exe 9280000 abcdefabcdefabcdefabcdefabcdefabcdefabcdefabcdefabcdefabcdefabcd
file=core/runtime.cb 10240 bcdefabcdefabcdefabcdefabcdefabcdefabcdefabcdefabcdefabcdefabcda
file=runtime.cb 10240 cdefabcdefabcdefabcdefabcdefabcdefabcdefabcdefabcdefabcdefabcdab
end
```

Example app bundle manifest:

```text
cflat-manifest-v1
scope=app-bundle
product=com.example.orders
channel=stable
sequence=105
version=2026.08.11.105
platform=source
min_compiler_sequence=72
created_utc=2026-08-11T00:00:00Z
entry=src/main.cb
artifact_name=orders-2026.08.11.105.cflatpkg
artifact_size=98304
artifact_sha256=2222222222222222222222222222222222222222222222222222222222222222
file=src/main.cb 12000 3333333333333333333333333333333333333333333333333333333333333333
file=src/lib/order.cb 8400 4444444444444444444444444444444444444444444444444444444444444444
end
```

The `.sig` file is detached:

```text
cflat-signature-v1
algorithm=ed25519
key_id=cflat-root-2026q3
signed_sha256=<sha256 of manifest.cflat bytes>
signature=<base64url Ed25519 signature over manifest.cflat bytes>
end
```

Verification must hash the manifest bytes, compare to `signed_sha256`, select the pinned
public key by `key_id`, enforce key scope and validity, then verify the Ed25519 signature over
the original manifest bytes.

## Key management

Trust store:

- embed at least two production public keys in code: the active key and the next rotation key;
- each key has `key_id`, `scope`, `not_before`, optional `not_after`, and `status`;
- keep development/test keys separate and disabled in release builds unless an explicit
  `--trust-test-key` style test flag is compiled only for non-production builds.

Rotation:

- new releases can be dual-signed during rotation;
- the updater accepts any valid signature from a trusted non-revoked key whose scope matches;
- a signed trust update can add a future key only when signed by an existing root key;
- emergency revocation is a signed metadata update that marks a key id revoked and is persisted
  locally.

Replay protection:

- persist the highest accepted `sequence` per `scope + product + channel`;
- reject lower sequences by default even if the signature is valid;
- allow manual rollback only to a locally installed known-good release, not to a newly
  downloaded lower sequence;
- protect update state with normal per-user or per-machine ACLs.

Clock handling:

- do not require the local clock for normal release acceptance if sequence is valid;
- use key expiry as a soft block only when trusted time is available;
- record server time from signed metadata for diagnostics.

## File verification for `.cb`

Because cflat has no file signature check today, add a verifier that sits before parsing:

- `cflat --verify-package <path>` verifies manifest, signature, archive hash, safe paths, and
  per-file hashes without compiling.
- `cflat --compile-package <path> -o <out>` verifies first, extracts to an immutable staging
  directory, then compiles only files named in the signed manifest.
- `cflat --run-package <path>` uses the same verifier before JIT/run, then reads source only
  through the verified package reader.
- OTA production mode accepts app inputs only through a verified package release slot.
- Plain `cflat foo.cb` remains available for local development, but the launcher does not use
  it for downloaded portal content.

Import handling for signed app bundles:

- the manifest declares the entry file and complete allowed source tree;
- imports must resolve inside the verified bundle or to trusted compiler core libraries;
- absolute imports, parent traversal, symlink escape, and import of unlisted files are compile
  errors in package mode;
- C interop inputs (`.c`, headers, libraries, package resolvers) are disabled in package mode
  unless the manifest explicitly opts into a signed dependency policy in a later version.

This gives cflat a practical file-integrity boundary without requiring every `.cb` file to carry
an individual signature. The signed manifest is the signature for the whole file set.

Verification is also part of compile and run, not only a separate diagnostic command. Do not
implement this as "verify paths, then let the normal compiler reopen those same paths later".
That leaves a time-of-check/time-of-use gap where a file can be swapped after verification and
before parsing.

Package mode must use one of these safe read models:

- Preferred: verify archive/member hashes, load every signed source file into immutable memory
  buffers, then compile or run from a `VerifiedPackage` file provider. Imports resolve by
  manifest path to those buffers, not by reopening arbitrary filesystem paths.
- Acceptable for large packages: extract into a private staging directory, reject symlinks and
  hardlinks, open source files with write sharing denied where the OS supports it, hash from
  the same file handle that will provide parser bytes, and pass compiler reads through a
  verifier-owned file provider. On POSIX, pair open file descriptors with `fstat` checks and
  never trust a reopened path.
- For activated app release slots, the launcher revalidates the signed manifest, artifact/file
  hashes, and the locally recorded build-output hashes before every production run.

The invariant is simple: once package mode says "trusted", every byte the parser or runtime
launcher consumes must be either already hashed in memory or rehashed from the exact open handle
being consumed.

## `--verify-package` detailed flow

`--verify-package` is a strict, read-only validator. It must not compile, execute package
hooks, resolve imports from the network, update local sequence state, activate a release, or
write extracted files into a live install directory. Its output is a trusted package report
that later commands can consume.

Preferred production packaging shape:

```text
orders.cflatpkg
orders.cflatpkg.manifest.cflat
orders.cflatpkg.sig
```

The manifest signs the archive hash, and the detached signature signs the manifest bytes.
This avoids the awkward circularity of embedding a manifest inside an archive whose hash is
also listed in that manifest. Directory verification can exist for diagnostics, but OTA
should consume the detached three-file shape.

Verification pipeline:

1. Open the package in read-only mode. Reject missing files, unsupported formats, archives
   larger than the configured maximum, and missing manifest or signature files.
2. Read `manifest.cflat` as raw bytes. Do not trim, normalize newlines, decode/re-encode, or
   pretty-print before verification.
3. Parse the manifest enough to enforce structural rules: magic line, LF-only newlines, no
   BOM, valid UTF-8, schema-ordered fields, no duplicate scalar fields, no unknown v1 fields,
   and final `end`.
4. Parse `manifest.cflat.sig`, hash the raw manifest bytes with SHA-256, compare the hash to
   `signed_sha256`, select the pinned public key by `key_id`, enforce key scope, and verify
   the Ed25519 signature over the original manifest bytes.
5. Validate signed policy fields: `scope=app-bundle`, expected `product` if supplied by the
   caller, allowed `channel`, non-replayed `sequence`, satisfied `min_compiler_sequence`,
   supported `platform`, and an `entry` that appears in the signed file list.
6. Stream-hash the outer package artifact and compare exact `artifact_size` and
   `artifact_sha256` from the signed manifest.
7. Inspect the archive directory before extraction. Reject absolute paths, drive letters,
   leading `/`, backslashes, NUL, empty path segments, `.`, `..`, symlinks, hardlinks, device
   entries, duplicate canonical paths, Windows/macOS case-fold collisions, too many entries,
   and expanded size above the configured maximum.
8. Compare the archive file set with the signed `file=` list. For v1, require exact match for
   payload files. Reject extra `.cb`, `.c`, header, library, script, or config files.
9. Stream every listed file and verify exact size and SHA-256. Only regular files can satisfy
   signed file entries.
10. Validate package-mode CFlat rules: the entry must be `.cb`; all source imports must resolve
    inside the signed file set or trusted compiler core libraries; absolute imports, parent
    traversal, symlink escape, and unlisted imports are errors; C interop and package resolver
    imports are disabled in v1.
11. Return a package report with `scope`, `product`, `channel`, `sequence`, `version`, `entry`,
    file count, artifact hash, signer key id, and policy verdict.

Example success output:

```text
OK: verified app-bundle com.example.orders stable sequence 105
entry: src/main.cb
files: 2
artifact_sha256: 2222222222222222222222222222222222222222222222222222222222222222
signed_by: portal-app-2026q3
```

Example failure output:

```text
error: package signature is valid, but file src/main.cb hash does not match manifest
```

For offline CI and forensic inspection, add `--ignore-local-state` to skip replay checks
against persisted local sequence state. It must not skip cryptographic checks, safe path
checks, artifact hash checks, file hash checks, or package-mode import policy.

`--compile-package`, `--run-package`, and launcher-run must call the same verification pipeline
and then consume the resulting `VerifiedPackage` object. The standalone `--verify-package`
command is only a report mode for that shared code path.

## Run-time verification

Production run has its own verification gate.

For source/JIT run:

1. Verify the package exactly as above.
2. Materialize signed source files as immutable buffers or verified open handles.
3. Compile/JIT from those buffers only.
4. Resolve imports through the package manifest, not by path lookup.
5. Refuse to continue if any file changes between hash and read.

For precompiled app run:

1. The package verifier authenticates the `.cb` source bundle before build.
2. The build step writes a local `build.manifest.cflat` containing hashes of generated
   executables, dynamic libraries, bitcode, and run configuration.
3. The launcher stores the build manifest under the immutable app release slot after compile
   succeeds.
4. Before every production launch, the launcher verifies the signed source manifest and the
   local build-output hashes.
5. If any hash fails, the launcher refuses to run that slot and rolls back to the previous
   known-good slot.

The local build manifest is not a publisher signature. It is a tamper-detection record tied to
the verified source release and protected by install-directory ACLs. A later version can add
publisher-signed prebuilt binaries, but v1 treats locally built outputs as derived artifacts.

## Cache interaction

OTA must not reuse stale compiler artifacts across incompatible releases.

Cache keys for package mode include:

- compiler sequence and version;
- runtime/core file hashes;
- target triple and platform;
- optimization/debug flags that affect output;
- app product, channel, sequence, and artifact hash.

Compiler release slots should use `--init-local` so each installed compiler carries its own
`.cflat` cache. App build output should live under the app release slot and be discarded with
that slot.

## CLI surface

Proposed commands:

- `cflat --update-check`: fetch signed metadata, print available update, do not install.
- `cflat --update-now`: install available compiler update, then ask launcher to relaunch.
- `cflat --update-status`: print current compiler/app versions, last check, and last failure.
- `cflat --rollback`: switch to the previous locally installed known-good release.
- `cflat --verify-package <path>`: verify a signed package without compiling.
- `cflat --compile-package <path> -o <out>`: verify and compile a signed app bundle.
- `cflat --run-package <path>`: verify and JIT/run a signed app bundle from verified bytes.
- `cflat --no-update`: skip auto-update for this invocation.

The launcher should own background auto-update. The compiler commands are for diagnostics,
CI, and explicit user actions.

## Failure behavior

- Bad signature, bad hash, unknown key, wrong scope, or replayed sequence: reject, quarantine
  the staged artifact, keep current release active.
- Network timeout or HTTP error: keep current release active, retry later with backoff.
- Extraction error or unsafe path: reject and quarantine.
- `--init-local` failure for a compiler release: reject the compiler release before activation.
- App compile failure: keep previous app release active and surface compiler errors normally.
- Launch crash or no readiness handshake: rollback pointer to previous release and mark the
  new release failed.
- Repeated failure of the same sequence: suppress automatic retries until a newer sequence or
  manual override appears.

## Implementation phases

1. Define manifest/signature parser and `SignatureVerifier` API with Ed25519 verification and
   SHA-256 streaming hash.
2. Add test vectors: valid signature, tampered manifest, tampered artifact, wrong key scope,
   replayed sequence, duplicate path, path traversal, unlisted file.
3. Add `--verify-package` for signed app bundles and compiler release archives.
4. Add safe extraction into staging directories and exact file-set verification.
5. Add package compile mode that restricts imports to signed files plus trusted core runtime.
6. Add immutable release-slot manager, update lock, state file, quarantine, and rollback.
7. Add launcher supervision and readiness handshake.
8. Add network fetcher with timeout, backoff, channel selection, and explicit update-site
   configuration.
9. Wire compiler self-update through the launcher; on Windows, never replace the running exe.
10. Add rollout controls: disable flag, canary channel, staged percentage, diagnostics, and
   operator-visible failure reason.

## Acceptance criteria

- A valid signed compiler release downloads, verifies, stages, initializes local cache,
  activates, launches, and becomes the persisted current release.
- A valid signed `.cb` app bundle verifies, compiles, activates, and launches.
- Any byte change in manifest, signature, archive, or listed `.cb` file is rejected.
- A valid app signature cannot authorize a compiler update.
- A valid compiler signature cannot authorize an app bundle unless separately trusted for that
  scope.
- A downloaded lower sequence is rejected even with a valid signature.
- Path traversal and symlink escape attempts are rejected before extraction writes outside the
  staging directory.
- Removing the network during update leaves the previous release active.
- A new release that fails readiness rolls back automatically.
- Plain local development compiles still work outside package/launcher production mode.

## Open decisions

- Choose the concrete crypto provider after checking existing dependencies. Do not modify
  `vcpkg.json` without explicit permission. Note: Windows CNG has no Ed25519 support through
  mainstream releases, so "platform crypto" is likely a dead end on Windows; the realistic
  choice is OpenSSL vs libsodium via vcpkg, both of which need explicit permission. Resolve
  this early - the SHA-256 stand-in proves nothing an attacker cannot recompute, and every
  later milestone silently depends on the swap being easy. Validate the `SignatureVerifier`
  abstraction claim by stubbing an Ed25519 backend interface now, even before it is wired.
- Decide whether update state is per-user or per-machine for the first production target.
- Define the launcher readiness contract for applications: process alive, TCP/pipe response,
  explicit `READY` stdout line, or app-specific health command.
- Decide whether signed app bundles may ever include C interop dependencies. The first version
  should keep this disabled.

## Review findings to resolve (2026-08-18)

Questions raised in design review; each needs an answer recorded here before the matching
implementation phase starts.

1. **Per-run verification cost.** Reverifying the active slot before every run means hashing
   `cflat.exe` plus every core file on every compile invocation - a startup tax that fights
   the `--init` cold-start work. Decide: accept and measure it, or cache the verification
   result keyed on (size, mtime, file identity) with a full rehash only on update, first run,
   or mismatch. Note that per-run rehash does not fully close TOCTOU on the launcher path
   anyway (the exe can be swapped between hash and process creation), so its marginal security
   over ACL-protected immutable slots is small relative to its cost.
2. **Replay/revocation state survives only as a local file.** `highest_accepted` lives in
   `update-state.json`, and corrupt-state recovery falls back to a slot scan that loses the
   highest-accepted sequence, reopening replay. The persisted key-revocation record has the
   same weakness: deleting the file un-revokes a key. Decide what the recovery path preserves,
   or state explicitly that local-file deletion resets replay/revocation state and why that is
   acceptable under the attacker model.
3. **`min_launcher_sequence` has no enforcement story.** Launcher self-update is a non-goal,
   and there is no defined behavior when a compiler release requires a newer launcher.
   Proposed: reject the release with a clear "launcher too old" error, and do not quarantine
   it as failed - retrying after a launcher update should succeed.
4. **No garbage collection.** Release slots accumulate (each with a full core tree and
   `--init-local` cache) plus quarantined failures. Define a retention policy (e.g. keep
   current + previous + N quarantined, prune the rest) before any real deployment.
5. **Consider not using zip.** Zip is an ambiguous format (duplicate central-directory
   entries, local-header vs central-directory disagreement, zip64 edge cases) - exactly the
   parser-divergence risk the strict manifest format avoids. Since we control packer and
   unpacker, a trivial flat container (magic, then length-prefixed path+bytes entries in
   manifest order) would remove that attack surface for less code than a hardened zip reader.
6. **Hash wording.** "Ed25519 with SHA-256" reads oddly (Ed25519 uses SHA-512 internally);
   the intent is SHA-256 for file/artifact hashes only. Clarify so nobody "fixes" it later.
7. **`--run-package` restrictions.** It inherits `--run` limits (single-threaded; programs
   using `thread<T>` or `program` are rejected). Document that such bundles must go through
   `--compile-package`.
8. **Windows path hazards.** Safe-path rules ban `:` and cover case-fold collisions, but
   should also explicitly ban Windows reserved device names (`CON`, `NUL`, `COM1`, ...) and
   trailing dots/spaces.
9. **Seeded-slot escape hatch in production.** The sibling `cflat.exe`-without-manifest
   seeding path is for local development only; state that it is compiled out or hard-disabled
   in production builds, same as test keys.
