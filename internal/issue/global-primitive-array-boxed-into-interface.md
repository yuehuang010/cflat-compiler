# A GLOBAL primitive array assigned to an interface is accepted and silently miscompiles

Filed 2026-07-28 during the `as`/`is` source-routing fix. PRE-EXISTING and unrelated to
that fix: this is the PLAIN assignment path, and it behaves identically on the pre-fix
binary.

Severity: SILENT MISCOMPILE. Accepted with no diagnostic, escapes the LLVM module
verifier, and segfaults on first use. The LOCAL spelling of the same code is caught by
the verifier, so only the global path is silent.

## Repro

```cflat
interface IShape { int area(); };
int[3] gInt;

extern int main()
{
    gInt[0] = 111;
    IShape s = gInt;      // accepted, no diagnostic
    printf("%d\n", s.area());
    return 0;
}
```

Exit 139. The array's first element is used as the vtable pointer: `gInt[0] = 111`
becomes the vtable and the dispatch dereferences 111.

Binding alone, without ever using the value, exits 0 - which is what makes this easy to
miss:

```cflat
IShape s = gInt;          // no use of s: exits 0, looks fine
```

The LOCAL spelling is NOT silent - it is caught, though only by the backend:

```cflat
extern int main() { int[3] a; IShape s = a; return 0; }
// Module verification failed: Invalid bitcast ptr to %__iface_fat_ptr, exit 1
```

## Root cause

Not diagnosed. `RejectPointerShapedInterfaceUpcast` (`MainListener.h:9971`) is what
rejects a CLASS array (`Circle[3]`) with a good message, and that guard evidently does
not fire for a PRIMITIVE element type. The global and local paths then diverge in
whether the resulting bad value reaches the verifier at all.

## Fix direction

Reject a primitive-element array boxed into an interface at the assignment path, with
the same wording `RejectPointerShapedInterfaceUpcast` already produces for a class
array. Note the message would need a primitive-aware variant: the existing text steers
the user to "a single instance pointer 'Circle*' or a 'Circle' value", which is not
meaningful advice for `int[3]` - no `int` can implement an interface, so the right
message says the element type cannot implement the interface at all.

## Why the `as` spelling was deliberately NOT changed to match

The `as`/`is` source-routing fix rejects `gInt as IShape` (it lands in the classifier's
`Unknown` category). That is strictly better than the miscompile, but it means `as` is
currently STRICTER than the plain spelling for this one shape. Making `as` match plain
would have meant re-accepting a miscompile; making plain match `as` is this issue. The
classifier deliberately gates its declared-type lookup on
`dataStructures.count(TypeName)` so primitives are excluded rather than silently picking
up the class-array wording - see the comment at `MainListener.h:12206-12207` and the
scope note in `Test/errors/err_as_array_source_interface.cb`.

Fixing this issue should remove that asymmetry; check both spellings agree afterwards.

## Related

[[interface-issue-queue]]
