# An empty brace list `= {}` never seeds the target, and SIGSEGVs the compiler as a parameter default

Filed 2026-08-02 while fixing `fix/ptr-fieldinit` (the pointer-brace-init P1, since fixed and deleted). Found by
enumerating the SYNTAX axis of that issue (`= {...}` vs bare `{...}` vs `{}`); it is a different root
cause and is deliberately NOT closed by that fix.

Severity: silent miscompile (uninitialized read, no diagnostic). The parameter-default face WAS a
compiler SIGSEGV (exit 139, zero output); `fix/ptr-fieldinit` added the null-`defaultVal` bail
described under "Fix direction", so as of that commit it is a located diagnostic with exit 1. The
SEEDING root cause is untouched and remains open - `= {}` still initializes nothing.

## Repro - measured on the `dd6f836` Release build (macOS arm64)

Local scalar declaration, `=` spelling, empty braces - the target is never written at all:

```cflat
extern int main(){ int x = {}; printf("%d\n", x); return 0; }        // prints garbage
```

`--no-opt` IR for `S* p = {};` (same shape, pointer target):

```
%p = alloca ptr, align 8
%0 = load ptr, ptr %p, align 8      ; no store anywhere - reads undef
```

The BARE-brace spelling of the same declaration DOES seed it, so the two spellings disagree.
Measured, both on `dd6f836`:

```
S* p = {};   ->  p=0x1f7808100   (undef; no store emitted)
S* p   {};   ->  p=0x0           (store ptr null)
S* gp = {};  ->  p=0x0           (global scope zero-inits)
```

As a PARAMETER DEFAULT the same empty list crashed the compiler, for every type tested. Measured
with `-o` (not `--run`, which cannot tell a compiler crash from a program crash) on the `dd6f836`
Release build, and again on `fix/ptr-fieldinit` after its bail landed:

```cflat
int f(int x = {})   { return x; }    // BEFORE rc 139, zero output   AFTER rc 1 + diagnostic
int f(S   s = {})   { return s.a; }  // BEFORE rc 139, zero output   AFTER rc 1 + diagnostic
int f(S*  p = {})   { return 0; }    // BEFORE rc 139, zero output   AFTER rc 1 + diagnostic
```

The AFTER message, verbatim for the first line:

```
r2_eb_int.cb(1,16): cannot build the default value for parameter 'x' of type 'int' - this default
initializer form is not supported; use '= default' or an explicit expression
```

This is containment, not a fix: the construct is still not supported, it just no longer segfaults.

## Root cause (hypothesis, cited but not measured to the crash site)

`{}` produces a NULL `initializerList()` context - the list rule requires at least one element.
The local declarator's seeding arm is gated on `initializer->initializerList() != nullptr`
(`MainListener.h` ~9478), so the `= {}` spelling matches none of the arms and `right` stays null:
nothing is stored. The bare-brace arm just below it is gated on the `LeftBrace()` token instead,
which is why that spelling seeds correctly - the two gates ask different questions about the same
construct.

The default-parameter wrapper (`MainListener.h` ~7819) has the same shape: `Default()`,
`assignmentExpression()` and `initializerList()` all miss, `defaultVal` stays null, and it was then
pushed into `namedVar.Primary` / `BaseType` and dereferenced downstream. That dereference is what
the `fix/ptr-fieldinit` bail now stands in front of.

## Fix direction

Gate the seeding on the brace TOKEN, not on the (optional) list context, in both the declarator and
the default-parameter paths - i.e. treat `= {}` exactly as `{}` already is: default/zero-init.

The null-`defaultVal` bail the default-parameter path needed is DONE (`fix/ptr-fieldinit`, per
CLAUDE.md's rule that a diagnosed compiler crash gets a proper error message). It converts the
segfault to a located diagnostic and nothing more; once the seeding is fixed, `= {}` should reach
the `= default` path and the bail should stop firing for it.

Do not fix this by rejecting `= {}`: three of the four neighbouring spellings (bare local, global,
fixed array) already accept it and zero-init, so a rejection would be the odd one out.
