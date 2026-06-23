# WinMD / COM Interop

CFlat can author, consume, and emit Windows Runtime (WinRT) / COM components:

- **Author** a COM object in CFlat with `[winrt] class` - it lowers to a real COM object
  (thin-pointer vtable, QueryInterface/AddRef/Release, the WinRT HRESULT ABI) callable by any
  COM/WinRT consumer.
- **Consume** WinRT metadata with `import "Foo.winmd";` - the interfaces, value structs, and enums
  become CFlat types you drive through the COM vtable.
- **Emit** a `.winmd` describing your `[winrt]` types with `--emit-winmd`, for tooling / projection.

This page covers what is implemented today. See the end for current limitations.

---

## Authoring a COM object: `[winrt] class`

A `[winrt]` class implements exactly one `[uuid]`-annotated interface and lowers to a thin COM
object: a struct whose first field is a vtable pointer, followed by a refcount and your fields.

```cflat
import "com.cb";

[uuid("4d57f3c2-8e1a-4b9d-9d3a-1f2e3c4b5a60")]
interface IPurr { i32 Eat(); };

[winrt] class Cat : IPurr
{
    i32 meals = default;
    Cat(i32 start) { this.meals = start; }
    i32 Eat() { this.meals = this.meals + 1; return 0; }
    ~Cat() { /* runs when the last reference is released */ }
};

extern int main()
{
    Cat* c = new Cat(3);          // alloc + wire vtable + refcount = 1
    HResult<i32> r = c->Eat();    // dispatch through the COM vtable
    c->Release();                 // refcount -> 0: runs ~Cat() and frees
    return r.ok() ? 0 : 1;
}
```

The compiler generates, per `[winrt]` class:

- the COM vtable (flat IInspectable layout): `QueryInterface`, `AddRef`, `Release`, `GetIids`,
  `GetRuntimeClassName`, `GetTrustLevel`, then your interface methods;
- a static vtable instance, wired into every object's `lpVtbl` by `new`;
- `QueryInterface` matching **IUnknown**, **IInspectable**, **IAgileObject**, and your interface's IID;
- thread-safe `AddRef`/`Release`; the final `Release` runs the destructor and frees.

Object lifetime is **reference-counted** (via `Release`), not owning-pointer auto-free - a `[winrt]`
object is never auto-deleted at scope exit. Create with `new`, drop with `Release`.

### Calling methods - the member-call sugar

`recv->Method(args)` dispatches through the vtable with an implicit `this`, hiding `lpVtbl`:

```cflat
HResult<i32> r = c->Eat();             // interface method -> HResult<T> (see the ABI below)
u32 rc = c->AddRef();                  // IUnknown method, reached by name
c->Release();
Guid iid = IID_IUnknown();
void* out = default;
i32 hr = c->QueryInterface(&iid, &out);
```

`QueryInterface`/`AddRef`/`Release` exist only in the vtable and are reachable **only** through this
sugar. The explicit form still works if you want it: `c->lpVtbl->Eat(c, &out)`.

---

## The HRESULT ABI and `HResult<T>`

At the COM ABI every interface method returns an `HRESULT`, and its logical return value comes back
through a trailing `[out, retval]` pointer. CFlat models that with `HResult<T>` (from `com.cb`),
which carries the status and the value together:

```cflat
struct HResult<T>
{
    i32 hr;          // S_OK (0) on success; a failure HRESULT otherwise
    T   value;       // valid only when ok()
    bool ok();       // hr >= 0
    bool failed();   // hr < 0
    void succeed(T v); void fail(i32 e);
};
```

You write the method body in one of two ways, and the compiler generates the right thunk:

```cflat
[winrt] class Counter : ICounter
{
    i32 total = default;

    // Infallible: just return a value. The thunk returns S_OK and writes the value out.
    i32 Add(i32 by) { this.total = this.total + by; return this.total; }

    // Fallible: return HResult<T> to report a failure HRESULT.
    HResult<i32> Checked(i32 by) {
        HResult<i32> r = default;
        if (by < 0) { r.fail(E_INVALIDARG); return r; }
        this.total = this.total + by;
        r.succeed(this.total);
        return r;
    }
};
```

Both emit the same winmd logical signature (`Int32 Add(Int32)`). On the calling side the member-call
sugar always yields an `HResult<T>` - **no auto-unwrap** - so you check the status explicitly:

```cflat
HResult<i32> r = k->Add(5);
if (r.failed()) { /* handle r.hr */ }
i32 total = r.value;
```

### Chaining with `?.`

`?.` short-circuits a chain of COM calls on the first failure. The chain stays an `HResult<U>`, so
you check once at the end (no early return - that's yours to write):

```cflat
HResult<i32> r = factory->Make()?.Value();   // Make() -> HResult<Counter*>, then Value()
if (r.failed()) { log(r.hr); return; }       // r.hr is the FIRST failing HRESULT
i32 v = r.value;                             // safe: the whole chain succeeded
```

If `Make()` fails, `Value()` is never called and `Make`'s HRESULT propagates straight to `r`.

---

## `com.cb` helpers

`import "com.cb";` brings in the shared COM values:

- HRESULT codes: `S_OK`, `S_FALSE`, `E_NOTIMPL`, `E_NOINTERFACE`, `E_POINTER`, `E_ABORT`, `E_FAIL`,
  `E_UNEXPECTED`, `E_ACCESSDENIED`, `E_HANDLE`, `E_OUTOFMEMORY`, `E_INVALIDARG`.
- `bool SUCCEEDED(i32 hr)` / `bool FAILED(i32 hr)`.
- `HResult<T>` (above).
- Well-known IIDs as functions: `IID_IUnknown()`, `IID_IInspectable()`, `IID_IAgileObject()`. (They
  are functions because a `Guid` global cannot be a compile-time constant.) Take the address for the
  `REFIID` an API expects: `Guid iid = IID_IUnknown(); ... QueryInterface(&iid, &out);`

---

## Consuming WinRT metadata: `import "Foo.winmd"`

`.winmd` is an import file type. The interfaces, value structs, and enums become CFlat types you
drive by hand through the COM vtable - exactly like the hand-written COM demos under `example/COM/`.

A bare name (`import "Windows.Foundation.winmd";`) resolves against the system WinRT metadata store
(`%SystemRoot%\System32\WinMetadata`), so the in-box winmds need no full path; a relative or absolute
path also works for a winmd you ship yourself.

```cflat
import "Windows.Foundation.winmd";       // resolved from %SystemRoot%\System32\WinMetadata

extern int main()
{
    i32 done = AsyncStatus_Completed;        // enum members -> named constants <Enum>_<Member>
    IStringable* s = default;                // an imported interface -> thin COM pointer struct
    if (s != nullptr) {
        void* str = default;
        i32 hr = s->lpVtbl->ToString(s, &str);   // call through the vtable, WinRT HRESULT ABI
        s->lpVtbl->Release(s);
    }
    return 0;
}
```

What is registered from a `.winmd`:

- **interfaces** (non-generic) -> a COM vtable struct `<Name>Vtbl` (IInspectable layout + the
  interface methods) plus a thin pointer struct `<Name> { <Name>Vtbl* lpVtbl }`. Each method slot is
  the WinRT ABI: returns HRESULT, `this` first, a trailing out-pointer for a non-void return.
- **value structs** -> CFlat structs.
- **enums** -> named constants `<EnumName>_<Member>`.

You get an interface pointer from a WinRT API call and drive it through `lpVtbl`. Scalar parameters
map precisely; `String`/`Object`/interfaces/arrays/by-ref map to opaque `void*` (the COM thin
pointer), which is ABI-correct.

### Calling consumed interfaces - the member-call sugar

The same `recv->Method(args)` sugar shown for the produce side applies to **consumed** interfaces -
both winmd-imported and [header-imported COM](C_INTEROP.md#com-interfaces-straight-from-a-header). A
vtable-slot name routes through `lpVtbl` and the receiver is supplied as the implicit `this`, so the
explicit example above can drop both the `->lpVtbl` step and the repeated receiver:

```cflat
void* str = default;
i32 hr = s->ToString(&str);   // sugar for: s->lpVtbl->ToString(s, &str)
s->Release();                 // sugar for: s->lpVtbl->Release(s)
```

The sugar fires on any **thin COM interface pointer** - a struct whose sole field is `lpVtbl` - which
is exactly the shape of both an imported winmd interface and a MIDL C-header interface. `QueryInterface`/
`AddRef`/`Release` live only in the vtable and are reachable by name through this sugar. The fully
explicit `s->lpVtbl->ToString(s, &str)` form always works too; reach for it when the receiver is an
rvalue with no storage (e.g. a freshly cast pointer `((IReference<int>*)boxed)->lpVtbl->QueryInterface(...)`),
which the storage-based sugar cannot rewrite.

---

## Parameterized (generic) interfaces: `IVector<int>`, `IReference<T>`, ...

WinRT's parameterized interfaces (`IVector<T>`, `IVectorView<T>`, `IMap<K,V>`, `IReference<T>`,
`IIterable<T>`, ...) are not stored per-instantiation in metadata - every projection *synthesizes*
each concrete instance, including its IID. CFlat does the same: write `IVector<int>` and the compiler
instantiates a concrete COM vtable + thin pointer from the generic template in the imported `.winmd`,
and derives the instance's IID (the **PIID**) by the standard RFC 4122 v5 / SHA-1 algorithm over the
WinRT type signature - byte-for-byte what cppwinrt and windows-rs compute.

```cflat
import "Windows.Foundation.winmd";

IReference<int>* r = default;            // resolves to a synthesized concrete COM interface
int v = default;
i32 hr = r->lpVtbl->get_Value(r, &v);    // drive it through the vtable like any imported interface
```

### `iidof(T)` - the derived IID

`iidof(T)` yields a `REFIID`-shaped pointer to the type's 16-byte IID - the **derived PIID** for a
parameterized interface, or the stored IID for a plain interface. Pass it straight to
`QueryInterface`:

```cflat
// boxed is an IInspectable* from a WinRT API (e.g. PropertyValue.CreateInt32).
IReference<int>* obj = (IReference<int>*)boxed;
IReference<int>* ref = default;
i32 qi = obj->lpVtbl->QueryInterface(obj, iidof(IReference<int>), (void**)&ref);
// qi == 0: the OS agreed on the IID. Now ref->lpVtbl->get_Value(ref, &out) works.
```

See `example/COM/winrt_ireference_demo.cb` for a full live round-trip (activate
`Windows.Foundation.PropertyValue`, box an `Int32`, QI to `IReference<int>` with the derived PIID,
read it back). Type arguments must be an explicit-width scalar (`i32`/`u32`/`f32`/`f64`/`bool`/`string`/
`object`, or the `int`/`uint`/`float`/`double` aliases) or an imported winmd type; arrays are not
permitted as type arguments (a WinRT rule).

### `foreach` over a WinRT collection

`for (T x in coll)` works when `coll` is any imported winmd interface that is (or *requires*)
`IIterable<T>` - `IVector<T>`, `IVectorView<T>`, `IIterable<T>` itself, and so on. The compiler
synthesizes `IIterable<T>` / `IIterator<T>` on demand, `QueryInterface`s the receiver for
`IIterable<T>` with the derived PIID, then drives `First` / `get_HasCurrent` / `get_Current` /
`MoveNext` and `Release`s the iterator afterwards:

```cflat
import "Windows.Foundation.winmd";   // IIterable`1 / IIterator`1 (from system WinMetadata)
import "Windows.Data.winmd";         // JsonArray, IJsonValue

// arr is an IJsonArray* (an IVector<IJsonValue>) from JsonArray.Parse("[10,20,30]").
double sum = 0.0;
for (IJsonValue* x in arr)
{
    double d = default;
    x->lpVtbl->GetNumber(x, &d);   // each element is a live IJsonValue
    sum = sum + d;
}
```

The element type is the loop variable's type (a scalar, or a named winmd type held by pointer for
interface elements). `Windows.Foundation.winmd` must be imported so the `IIterable`/`IIterator`
templates are available. See `example/COM/winrt_foreach_demo.cb` for the full live program. A
receiver that does not actually implement `IIterable<T>` fails the runtime `QueryInterface` and
iterates zero times (there is no compile-time iterability check yet).

> Projecting `IVector<T>`/`IMap<K,V>` onto CFlat's core `list`/`dictionary` is not done - use the
> interfaces directly.

---

## Emitting a `.winmd`: `--emit-winmd`

After a successful compile, `--emit-winmd <path>` writes a valid ECMA-335 `.winmd` describing the
program's `[winrt]` interfaces and classes (IIDs, method signatures, runtime classes):

```bash
x64/Debug/cflat.exe app.cb -o app.exe --emit-winmd app.winmd
```

The emitted file round-trips through the reader (`--dump-winmd`) and disassembles in `ildasm`. The
metadata's logical signatures match the generated HRESULT ABI, so the type contract is publishable
for tooling / projection.

---

## CLI flags

| Switch | Value | Description |
|--------|-------|-------------|
| `--emit-winmd` | path | After compiling, write the program's `[winrt]` types to a `.winmd`. |
| `--dump-winmd` | path | Read a `.winmd` into the projection model and print it (diagnostic), then exit. |
| `--winmd-instantiate` | path | Import a `.winmd` and instantiate well-known parameterized interfaces (`IVector<i32>`, `IReference<i32>`, ...), checking each derived PIID + vtable shape, then exit. |
| `--winmd-sig-selftest` | | Validate the parameterized-type signature encoder + PIID derivation against published reference IIDs and the RFC 4122 v5 vector, then exit. |
| `--check` | | A `.winmd` passed to `--check` is parsed-verified (no registration). `test_winmd.bat` uses this to batch-validate every SDK `.winmd`. |

A `.winmd` brought in via `import "x.winmd";` registers its types (consume). A `.winmd` passed to
`--dump-winmd`/`--check` is only read/verified.

---

## Current limitations

- **No activation factory / DLL output.** A `[winrt]` object is created in-process with `new`. The
  activation factory + `DllGetActivationFactory` (for an external WinRT host to `RoActivateInstance`
  across a DLL boundary) is intentionally not generated - CFlat does not output DLLs.
- **Parameterized (generic) WinRT interfaces** (`IVector<T>`, `IReference<T>`, ...) are instantiated
  on consume and support `foreach` (see above), but projection onto core `list`/`dictionary` is not
  yet provided; non-iterating method calls drive the COM vtable by hand.
- **Properties / events** are surfaced as their underlying methods (`get_X`/`put_X`, `add_X`/
  `remove_X`), not as `obj.X` / `obj.E += h` sugar.
- A `[winrt]` class implements a **single** primary interface; `requires`/multi-interface composition
  is not yet generated.
- Interface methods returning a struct via the sret ABI, and fallible `void` methods, are not yet
  supported by the produce thunk (diagnosed, not miscompiled).
- The WinRT rule that `[winrt]` types live in a namespace is not yet enforced (file-scope compiles).
