# A C `#define A B` alias binds to a CFLAT function named B, not only to C declarations

Filed 2026-08-22 from the round-2 review of the C-interop alias follow-ups. Pre-existing on
master (`import "mod.cb"; import "alias.h";` binds there too); the pending-alias retry only widens
the ordering window (alias-header-first now binds as well).

## Repro

```
// mod.cb
int Real(int x) { return x + 1; }
// alias.h
#define Alias Real
// main.cb
import "mod.cb";
import "alias.h";
extern int main() { return Alias(1); }   // binds to the CFlat Real
```

A plain `.cb`-local function still does NOT bind (imports are processed before CFlat function
registration), so the behaviour is also order-dependent between imported and local CFlat code.

## Root cause

`RegisterCMacroAliases` resolves the alias target against `functionTable`, which holds CFlat
symbols as well as C declarations.

## Fix direction

Resolve alias targets only against entries flagged as C declarations (the C-interop registration
already marks them); keep the pending table for late C targets. Leg: expect_error for the CFlat
target shape plus a value leg proving the C target still binds. Low priority: harmless unless a
CFlat and a C function share a name.
