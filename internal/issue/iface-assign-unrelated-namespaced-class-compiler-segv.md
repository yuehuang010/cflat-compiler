# Compiler SIGSEGV assigning a namespaced class to an unrelated file-scope interface

Found 2026-07-27 by the independent review of `fix/iface-ifconst` (reported there as D2).
NOT caused by that branch - CONFIRMED on master `853cb87` with the Release binary.
No `if const` is involved.

## Repro

```cflat
interface IHandleRoot { int root(); };
interface IHandle : IHandleRoot { int h(); };

namespace Win32
{
    interface IHandle { int nativeHandle(); };
    class WinFile : Win32.IHandle
    {
        int v = 0;
        WinFile() {}
        int nativeHandle() { return v; }
    };
};

extern int main()
{
    Win32.WinFile w = Win32.WinFile();
    IHandleRoot e = w;          // WinFile implements Win32.IHandle, NOT IHandleRoot
    return e.root();
}
```

```
$ x64/Release/cflat repro.cb -i Test/library --check
(no output)
exit code 139   (SIGSEGV)
```

Reproduces under `--check`, so it does not need codegen or linking.

## What SHOULD happen

A clean `LogError`. `Win32.WinFile` implements `Win32.IHandle`, which is a DIFFERENT
interface from the file-scope `IHandle` and unrelated to `IHandleRoot`. The conversion is
invalid and should be rejected with a diagnostic naming both types.

## Why it is interesting

Two same-named interfaces in different namespaces (`IHandle` and `Win32.IHandle`) are
exactly what commit `9498562` made legal and distinct. The conversion check appears to
match the class against the interface by a name that is not fully qualified, then
dereference a vtable/contract that was never built for it. Per CLAUDE.md, once the root
cause is known this needs a real error message rather than a crash.

Likely related: [[bare-interface-name-resolves-outward-before-namespace]] - the same
family of bare-vs-qualified interface name confusion introduced by the namespace work.
Check whether one fix covers both.

## Fix direction

Undiagnosed. Start under a debugger at the interface-assignment conversion path
(`IHandleRoot e = w;`) and find where an unqualified `IHandle` is used to look up a
contract that belongs to `Win32.IHandle`. Add the missing rejection, and a regression
test in `Test/errors/`.
