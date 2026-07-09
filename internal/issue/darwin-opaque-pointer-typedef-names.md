# Darwin header bind: opaque-struct-pointer typedef NAMES not usable

Summary
-------
On the macOS C-header binding path (S3), a typedef of the shape
`typedef struct Foo *FooRef;` where `struct Foo` is only ever forward-declared
(opaque) does NOT register `FooRef` as a usable CFlat type alias. The functions
that use it still auto-extern fine - the opaque pointer maps to `void*` in their
signatures - but a user cannot spell `FooRef` as a variable/parameter type.

Repro
-----
```cflat
import package "CoreGraphics/CoreGraphics.h" framework "CoreGraphics";
extern int main()
{
    CGColorSpaceRef cs = CGColorSpaceCreateDeviceRGB();  // ERROR: unknown type 'CGColorSpaceRef'
    return 0;
}
```
Workaround (used by example/macos/framework_link.cb): declare the variable with the
mapped primitive `void*`:
```cflat
void* cs = CGColorSpaceCreateDeviceRGB();               // OK - auto-externed as void*
u32   n  = CGColorSpaceGetNumberOfComponents(cs);        // OK
CGColorSpaceRelease(cs);                                  // OK
```
`--symbol CGColorSpaceCreateDeviceRGB` shows `void* CGColorSpaceCreateDeviceRGB()`,
so the function is bound; only the typedef NAME `CGColorSpaceRef` is missing.

Root cause (direction)
----------------------
The typedef target is a pointer to a never-defined `struct CGColorSpace`. The
alias-adoption pass (AdoptTypedefs / RegisterCRecords in LLVMBackend.h) appears to
drop the alias when its underlying record is an opaque forward-decl rather than
emitting an opaque-pointer alias. Note Windows `HWND` (typedef struct HWND__*) DOES
work, so compare the Windows adoption path against the Darwin one - likely the
opaque forward record for the CG structs is not being registered/adopted the same
way (the framework siblings are angle-included, so the record's presumed file /
in-scope status may differ from the Win SDK layout).

Fix direction
-------------
When a typedef resolves to `struct X *` and `struct X` is opaque (no definition in
scope), register `FooRef` as an alias to an opaque `void*`-backed pointer type
(same net type the function signatures already use), instead of dropping it.

Deferred: not blocking S3 acceptance (auto-extern proven via void*). Delete this
file when opaque-pointer typedef names bind on the Darwin header path.
