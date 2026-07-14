# WinMD / COM Interop

CFlat can author, consume, and emit Windows Runtime (WinRT) / COM components:

- **Author** a COM object in CFlat with `[winrt] class` - it lowers to a real COM object
  (thin-pointer vtable, QueryInterface/AddRef/Release, the WinRT HRESULT ABI) callable by any
  COM/WinRT consumer.
- **Consume** WinRT metadata with `import "Foo.winmd";` - the interfaces, value structs, and enums
  become CFlat types you drive through the COM vtable.
- **Emit** a `.winmd` describing your `[winrt]` types with `--emit-winmd`, for tooling / projection.

This page covers what is implemented today. See the end for current limitations.

## Contents

- [WinRT Object Authoring](#winrt-object-authoring)
  - [The Member-Call Sugar](#the-member-call-sugar)
  - [The HRESULT ABI (`HResult<T>`)](#the-hresult-abi-hresultt)
    - [Short-Circuit Chaining](#short-circuit-chaining)
  - [`com.cb` Helpers](#comcb-helpers)
- [WinRT Metadata Consumption](#winrt-metadata-consumption)
  - [The Member-Call Sugar on Imports](#the-member-call-sugar-on-imports)
  - [Parameterized (Generic) Interfaces](#parameterized-generic-interfaces)
    - [`iidof(T)` and Derived IIDs](#iidoft-and-derived-iids)
    - [`foreach` Iteration](#foreach-iteration)
  - [The `winrt.cb` Runtime](#the-winrtcb-runtime)
    - [The `lib { }` Clause](#the-lib---clause)
    - [`wstring` (UTF-16 Strings)](#wstring-utf-16-strings)
    - [`awaitAsync` (Async Operations)](#awaitasync-async-operations)
- [`.winmd` Emission](#winmd-emission)
- [Reference](#reference)
  - [CLI Flags](#cli-flags)
  - [Not Yet Implemented](#not-yet-implemented)

---

## WinRT Object Authoring

A `[winrt]` class implements exactly one `[uuid]`-annotated interface and lowers to a thin COM
object: a struct whose first field is a vtable pointer, followed by a refcount and your fields.

The `[winrt]` and `[uuid(...)]` annotations are declared in `com.cb` (as ordinary `annotation`
definitions), so authoring a COM object requires `import "com.cb";` - using them without it is an
"Unknown annotation" error.

```cpp
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

### The Member-Call Sugar

`recv->Method(args)` dispatches through the vtable with an implicit `this`, hiding `lpVtbl`:

```cpp
HResult<i32> r = c->Eat();             // interface method -> HResult<T> (see the ABI below)
u32 rc = c->AddRef();                  // IUnknown method, reached by name
c->Release();
Guid iid = IID_IUnknown();
void* out = default;
i32 hr = c->QueryInterface(&iid, &out);
```

`QueryInterface`/`AddRef`/`Release` exist only in the vtable and are reachable **only** through this
sugar. The explicit form still works if you want it: `c->lpVtbl->Eat(c, &out)`.

### The HRESULT ABI (`HResult<T>`)

At the COM ABI every interface method returns an `HRESULT`, and its logical return value comes back
through a trailing `[out, retval]` pointer. CFlat models that with `HResult<T>` (from `com.cb`),
which carries the status and the value together:

```cpp
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

```cpp
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

```cpp
HResult<i32> r = k->Add(5);
if (r.failed()) { /* handle r.hr */ }
i32 total = r.value;
```

#### Short-Circuit Chaining

`?.` short-circuits a chain of COM calls on the first failure. The chain stays an `HResult<U>`, so
you check once at the end (no early return - that's yours to write):

```cpp
HResult<i32> r = factory->Make()?.Value();   // Make() -> HResult<Counter*>, then Value()
if (r.failed()) { log(r.hr); return; }       // r.hr is the FIRST failing HRESULT
i32 v = r.value;                             // safe: the whole chain succeeded
```

If `Make()` fails, `Value()` is never called and `Make`'s HRESULT propagates straight to `r`.

#### Who Releases the Intermediate?

Chaining through a factory - `factory->Make()?.Value()` - dispatches `Value()` on a `Counter*` the
caller never names. COM says an `[out, retval]` interface pointer comes back **AddRef'd** (`+1`), so
that intermediate is a reference nobody else holds. The chain therefore **releases it for you**,
right after it has dispatched through it:

```cpp
HResult<i32> r = factory->Make()?.Value();   // the Counter that Make() produced is released here
```

This happens **only** for the shape whose `+1` is guaranteed: an unnamed temporary `HResult<T*>`
that came straight out of a `[winrt]` vtable slot call, where `T` is a `[winrt]` class. The chain
never releases anything else, because nothing else promises a `+1`:

| Shape | Released by the chain? |
|---|---|
| `factory->Make()?.Value()` - temporary from a COM slot call | **Yes** - it is unreachable otherwise |
| `HResult<Counter*> h = factory->Make();` - you named it | No - it is yours; call `h.value->Release()` |
| `MyPlainFunc()?.Value()` - a plain (non-COM) function's `HResult` | No - it may hold a borrowed pointer |
| `HResult<i32>` and other non-pointer `T` | No - nothing to release |
| The chain's own **result** (`HResult<Cat*> c = f->Make()?.Adopt();`) | No - `c.value` is handed to you, so `c.value->Release()` is yours |
| A failing chain (`Make()` returned an error) | No - no object was produced |

So the rule stays the familiar COM one: **you release what a call hands you and you can still name.**
The chain only cleans up the references it created and then dropped on your behalf.

### `com.cb` Helpers

`import "com.cb";` brings in the shared COM values:

- HRESULT codes: `S_OK`, `S_FALSE`, `E_NOTIMPL`, `E_NOINTERFACE`, `E_POINTER`, `E_ABORT`, `E_FAIL`,
  `E_UNEXPECTED`, `E_ACCESSDENIED`, `E_HANDLE`, `E_OUTOFMEMORY`, `E_INVALIDARG`.
- `bool SUCCEEDED(i32 hr)` / `bool FAILED(i32 hr)`.
- `HResult<T>` (above).
- Well-known IIDs as functions: `IID_IUnknown()`, `IID_IInspectable()`, `IID_IAgileObject()`. (They
  are functions because a `Guid` global cannot be a compile-time constant.) Take the address for the
  `REFIID` an API expects: `Guid iid = IID_IUnknown(); ... QueryInterface(&iid, &out);`

---

## WinRT Metadata Consumption

`.winmd` is an import file type. The interfaces, value structs, and enums become CFlat types you
drive by hand through the COM vtable - exactly like the hand-written COM demos under `example/COM/`.

A bare name (`import "Windows.Foundation.winmd";`) resolves against the system WinRT metadata store
(`%SystemRoot%\System32\WinMetadata`), so the in-box winmds need no full path; a relative or absolute
path also works for a winmd you ship yourself.

### WinMD Types Are Always Fully Qualified

A type from a `.winmd` is registered under its **fully-qualified WinRT name** and nothing else -
`Windows.Foundation.IStringable`, never a bare `IStringable`. Consequently a WinMD projection can
never displace a type of your own: your `interface IStringable` and WinRT's are different types and
can coexist in one program. (They could not before: the projection silently won the lookup, and a
CFlat interface value - a fat pointer - was then emitted as a WinRT COM struct access, producing
invalid LLVM IR with no diagnostic.)

Use a `using` alias to get the short spelling back. The alias is *your* name, chosen in your own
file, so nothing can claim it behind your back - and if it clashes with one of your interfaces, the
compiler says so instead of silently shadowing it.

```cpp
import "Windows.Foundation.winmd";

using IStringable = Windows.Foundation.IStringable;    // plain interface
using IReference  = Windows.Foundation.IReference;     // GENERIC BASE: now write IReference<int>
using RefInt      = Windows.Foundation.IReference<int>;// or alias a concrete instantiation
```

An alias may name: a plain interface, a value struct, an enum, a delegate, a runtime class, a
generic *base* (the `<...>` is supplied at the use site), or a concrete generic *instantiation*.
Aliases chain, so `using PV = IPropertyValueStatics;` over an earlier alias works.

Two spots still take the bare short name, by design:

- **Enum members** are named constants `<Enum>_<Member>` (`AsyncStatus_Completed`). They are
  *values*, not types, so they cannot reach the bad-IR class above; and a dotted name in expression
  position is member access, so a qualified spelling would not even parse. First-writer-wins.
- **Type arguments** inside `<...>` are matched after alias expansion, so `IReference<HttpProgress>`
  works via `using HttpProgress = Windows.Web.Http.HttpProgress;`.

```cpp
import "Windows.Foundation.winmd";       // resolved from %SystemRoot%\System32\WinMetadata

using IStringable = Windows.Foundation.IStringable;

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

- **interfaces** (non-generic) -> a COM vtable struct `<FullName>Vtbl` (IInspectable layout + the
  interface methods) plus a thin pointer struct `<FullName> { <FullName>Vtbl* lpVtbl }`. Each method
  slot is the WinRT ABI: returns HRESULT, `this` first, a trailing out-pointer for a non-void return.
- **value structs** -> CFlat structs, under the full name.
- **enums** -> named constants `<EnumName>_<Member>` (the short name; see above).

You get an interface pointer from a WinRT API call and drive it through `lpVtbl`. Scalar parameters
map precisely; `String`/`Object`/interfaces/arrays/by-ref map to opaque `void*` (the COM thin
pointer), which is ABI-correct.

### The Member-Call Sugar on Imports

The same `recv->Method(args)` sugar shown for the produce side applies to **consumed** interfaces -
both winmd-imported and [header-imported COM](C_INTEROP.md#com-interfaces-straight-from-a-header). A
vtable-slot name routes through `lpVtbl` and the receiver is supplied as the implicit `this`, so the
explicit example above can drop both the `->lpVtbl` step and the repeated receiver:

```cpp
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

### Parameterized (Generic) Interfaces

WinRT's parameterized interfaces (`IVector<T>`, `IVectorView<T>`, `IMap<K,V>`, `IReference<T>`,
`IIterable<T>`, ...) are not stored per-instantiation in metadata - every projection *synthesizes*
each concrete instance, including its IID. CFlat does the same: write `IVector<int>` and the compiler
instantiates a concrete COM vtable + thin pointer from the generic template in the imported `.winmd`,
and derives the instance's IID (the **PIID**) by the standard RFC 4122 v5 / SHA-1 algorithm over the
WinRT type signature - byte-for-byte what cppwinrt and windows-rs compute.

A generic base is qualified like any other WinMD name. Alias the base once and the use sites read
exactly like any other projection:

```cpp
import "Windows.Foundation.winmd";

using IReference = Windows.Foundation.IReference;   // alias the BASE; <...> comes at the use site

IReference<int>* r = default;            // resolves to a synthesized concrete COM interface
int v = default;
i32 hr = r->lpVtbl->get_Value(r, &v);    // drive it through the vtable like any imported interface

// The fully-qualified spelling always works too, alias or no alias:
Windows.Foundation.IReference<int>* r2 = default;
```

#### `iidof(T)` and Derived IIDs

`iidof(T)` yields a `REFIID`-shaped pointer to the type's 16-byte IID - the **derived PIID** for a
parameterized interface, or the stored IID for a plain interface. Pass it straight to
`QueryInterface`:

```cpp
// boxed is an IInspectable* from a WinRT API (e.g. PropertyValue.CreateInt32).
IReference<int>* obj = (IReference<int>*)boxed;
IReference<int>* ref = default;
i32 qi = obj->lpVtbl->QueryInterface(obj, iidof(IReference<int>), (void**)&ref);
// qi == 0: the OS agreed on the IID. Now ref->lpVtbl->get_Value(ref, &out) works.
```

See `example/COM/winrt_ireference_demo.cb` for a full live round-trip (activate
`Windows.Foundation.PropertyValue`, box an `Int32`, QI to `IReference<int>` with the derived PIID,
read it back). Type arguments must be an explicit-width scalar (`i32`/`u32`/`f32`/`f64`/`bool`/`string`/
`object`, or the `int`/`uint`/`float`/`double` aliases) or an imported winmd type named by its full
name or a `using` alias of one; arrays are not permitted as type arguments (a WinRT rule).

#### `foreach` Iteration

`for (T x in coll)` works when `coll` is any imported winmd interface that is (or *requires*)
`IIterable<T>` - `IVector<T>`, `IVectorView<T>`, `IIterable<T>` itself, and so on. The compiler
synthesizes `IIterable<T>` / `IIterator<T>` on demand, `QueryInterface`s the receiver for
`IIterable<T>` with the derived PIID, then drives `First` / `get_HasCurrent` / `get_Current` /
`MoveNext` and `Release`s the iterator afterwards:

```cpp
import "winrt.cb";              // brings in Windows.Foundation.winmd (IIterable`1 / IIterator`1) + WinRT libs
import "Windows.Data.winmd";   // JsonArray, IJsonValue

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
interface elements). `Windows.Foundation.winmd` must be available so the `IIterable`/`IIterator`
templates can be synthesized - `import "winrt.cb";` pulls it in (and links the WinRT libs), or
import it directly. See `example/COM/winrt_foreach_demo.cb` for the full live program. A
receiver that does not actually implement `IIterable<T>` fails the runtime `QueryInterface` and
iterates zero times (there is no compile-time iterability check yet).

> Projecting `IVector<T>`/`IMap<K,V>` onto CFlat's core `list`/`dictionary` is not done - use the
> interfaces directly.

### The `winrt.cb` Runtime

Driving WinRT by hand means the same plumbing every program repeats: initialize the apartment,
marshal strings to/from `HSTRING`, fetch an activation factory, and wait on async operations.
`core/winrt.cb` packages all of it. Importing it also **imports `Windows.Foundation.winmd`**
(the `IReference`1` / `IIterable`1` / `IAsync*` foundation every WinRT API builds on) **and links
the WinRT import libs** (`RuntimeObject.lib`, `ole32.lib`), so a consumer needs only:

```cpp
import "winrt.cb";                 // + Windows.Foundation.winmd + WinRT libs, no lib clause needed
import "Windows.Storage.winmd";    // plus the .winmd of whatever specific API you call
```

What `winrt.cb` provides:

- `winrtInit()` / `winrtUninit()` - apartment lifecycle (`RO_INIT_MULTITHREADED`).
- `activationFactory(className, iid, out)` - build the class-name `HSTRING`, call
  `RoGetActivationFactory`, free the string, return the HRESULT. Pass `iidof(TFactory)` for `iid`.
- `hstring(string) -> void*` / `fromHString(void*) -> string` / `freeHString(void*)` - the
  `HSTRING` bridge (UTF-8 <-> UTF-16, built on `wstring` below). You own an `HSTRING` from
  `hstring()` and must `freeHString` it.
- `awaitAsync(void*) -> i32` - block on an `IAsyncAction*` / `IAsyncOperation<T>*` (see Async below).
- `asyncError(void*) -> i32` - the failure HRESULT (`IAsyncInfo.ErrorCode`) when `awaitAsync` did
  not return `1`; `0` if there was no error.

```cpp
winrtInit();
IPathIOStatics* pathIO = nullptr;
i32 hr = activationFactory("Windows.Storage.PathIO", iidof(IPathIOStatics), (void**)&pathIO);
```

#### The `lib { }` Clause

`winrt.cb` hides the link step, but if you import a `.winmd` yourself you supply the runtime libs
with a `lib` clause on the import - the same clause used for header binds:

```cpp
import "Windows.Foundation.winmd" lib { "RuntimeObject.lib", "ole32.lib" };
```

#### `wstring` (UTF-16 Strings)

CFlat's `string` is UTF-8 (byte-oriented); Windows wide APIs (`...W`) and `HSTRING` want UTF-16.
`core/wstring.cb` is its UTF-16 complement: an **owning value type** holding a heap buffer of `u16`
code units and a count. Conversion is a real UTF-8 <-> UTF-16 transcode via the OS (surrogate
pairs and all), not a byte-widen, so non-ASCII text round-trips correctly.

```cpp
import "wstring.cb";   // (winrt.cb already pulls this in)

wstring w = s.toWString();          // string (UTF-8) -> owned wstring (UTF-16)
u16* p   = w.data();                // NUL-terminated UTF-16 buffer for a ...W API
i32  n   = w.length();              // code-unit count (excludes the terminator)
string back = w.toString();         // wstring -> owned UTF-8 string
wstring fromRaw = wstring.fromUtf16(rawU16, len);   // take ownership of a transient u16* (e.g. an HSTRING buffer)
```

Like every owning value type, a `wstring` frees its buffer on scope exit and is **move-by-default**
(`wstring b = a` transfers the buffer and consumes `a`; use `b = a.copy()` for an independent copy).
Unlike `string`, it has no compiler support (no wide literals) - it is a pure library type.

#### `awaitAsync` (Async Operations)

Most of the useful WinRT surface is asynchronous - a method hands back an `IAsyncAction` (no result)
or an `IAsyncOperation<T>` (a future `T`). The simplest correct way to consume one without a
completion-handler delegate is to **poll**: `awaitAsync` `QueryInterface`s the object to `IAsyncInfo`
and spins on `get_Status` until it leaves the `Started` state, then returns the terminal
`AsyncStatus` (`Completed`=1, `Canceled`=2, `Error`=3, or -1 if `IAsyncInfo` was unavailable).

```cpp
void* readOp = nullptr;
pathIO->ReadTextAsync(hPath, &readOp);          // -> IAsyncOperation<String>
if (awaitAsync(readOp) == 1) {
    void* hOut = nullptr;
    ((IAsyncOperation<string>*)readOp)->lpVtbl->GetResults(readOp, &hOut);
    string text = fromHString(hOut);
    freeHString(hOut);
}
((IAsyncOperation<string>*)readOp)->lpVtbl->Release(readOp);
```

When `awaitAsync` returns anything other than `1` (Completed), the failure HRESULT lives on the
op's `IAsyncInfo.ErrorCode`. `asyncError(void*) -> i32` does that `QueryInterface` + `get_ErrorCode`
for you (returning `0` when there was no error), so a caller never has to hand-write it:

```cpp
i32 status = awaitAsync(readOp);
if (status != 1) {
    printf("async failed: status=%d hr=0x%08X\n", status, asyncError(readOp));
    return 1;
}
```

`IAsyncOperation<String>` is not stored in metadata; CFlat synthesizes its vtable and parameterized
IID on demand from the generic `IAsyncOperation`1` template in `Windows.Foundation.winmd`, exactly as
for `IReference<int>`. See `example/COM/winrt_async_demo.cb` for a full `PathIO` write/read round-trip.

> Async **delegates / completion handlers** are not projected - polling with `awaitAsync` is the
> supported model.

---

## `.winmd` Emission

After a successful compile, `--emit-winmd <path>` writes a valid ECMA-335 `.winmd` describing the
program's `[winrt]` interfaces and classes (IIDs, method signatures, runtime classes):

```bash
x64/Debug/cflat.exe app.cb -o app.exe --emit-winmd app.winmd
```

The emitted file round-trips through the reader (`--dump-winmd`) and disassembles in `ildasm`. The
metadata's logical signatures match the generated HRESULT ABI, so the type contract is publishable
for tooling / projection.

---

## Reference

### CLI Flags

The WinMD / COM switches (`--emit-winmd`, `--dump-winmd`, `--winmd-instantiate`,
`--winmd-sig-selftest`, and `--check` on a `.winmd`) are documented in the command-line reference:
[`doc/CLI.md` -> WinMD / COM](CLI.md#winmd--com).

### Not Yet Implemented

- **No activation factory / DLL output (produce side).** A `[winrt]` object *you author* is created
  in-process with `new`; the activation factory + `DllGetActivationFactory` (for an external WinRT
  host to `RoActivateInstance` across a DLL boundary) is intentionally not generated - CFlat does not
  output DLLs. (Consuming a system component's factory is fully supported - see `winrt.cb`'s
  `activationFactory`.)
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
