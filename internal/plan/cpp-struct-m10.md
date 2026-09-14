# M10: `[cpp] struct` - a CFlat struct that IS a C++ class

Parent plan: `internal/plan/cpp-direct-abi-interop.md` (M10 section holds the rulings).
Status: v1 implemented 2026-09-13 (working tree, uncommitted); runs A (foundation), B (base + override),
C/E (review closures), D (template-form diagnostics + catalog). Record in the parent plan's M10 entry.

## Rulings (maintainer, 2026-09-13)

1. A `[cpp] struct` is a real C++ class/struct that follows C++ rules. CFlat owns as little of
   the logic as possible and defers to clang: clang emits layout, vtable, type_info, structor
   chaining and the base subobject. CFlat never re-implements C++ class semantics.
2. No `virtual` keyword in CFlat. Virtual-ness is deduced from the C++ base (whole program is
   visible). Writing `virtual` is a compile error that says so.
3. `override` is required on every method that overrides a base virtual. `override` that
   overrides nothing is a compile error. A method that would HIDE a base virtual without
   `override` is a compile error.
4. Copy follows CFlat borrow-by-default: implicit copy is refused, transfer needs explicit
   `move`. A moved-from `[cpp] struct` is compile-time consumed; its C++ destructor still runs
   (C++ rule), on a CFlat field block that the move zeroed.
5. Open, decide from real programs: whether `std::unique_ptr<T>` members map onto `unique<T>`.

## Surface

```cflat
import cpp "library/cpp_interop_poly.h";

[cpp] struct Plain                 // no base: a C++ class holding only CFlat fields
{
    int a = 1;
    string name = default;
    Plain(int v) { a = v; }
    ~Plain() { }
    int total() { return a * 2; }
};

struct MyCircle : cpppoly.Shape     // a C++ base makes the struct [cpp] automatically;
{                                   // writing [cpp] as well is accepted
    int r = 3;
    MyCircle(int rr) : cpppoly.Shape() { r = rr; }   // base initializer, C++ spelling
    override int area() { return r * r; }            // must override a base virtual
};
```

- `[cpp]` is an annotation declared in core (`annotation cpp { };`, like `unique`).
- A struct with a base clause must name exactly one C++ class (v1: multiple bases, virtual
  bases, CFlat structs, interfaces as bases -> error). A `[cpp] struct` without a base is legal.
- Constructor base initializer: `Name(params) : <base spelling>(args) { ... }`. Omitted -> the
  base is default-constructed. On a non-`[cpp]` struct the initializer is an error.
- `override` is a soft keyword in prefix position of a member function (both passes text-match
  it, never a lexer token). Allowed only inside a `[cpp] struct` with a base.
- Generic `[cpp] struct` -> error (v1). Nested in a namespace -> allowed.

## Mechanism (what clang emits, what CFlat emits)

For `[cpp] struct Foo : Base` the main pass, at `ParseStructDefinition`, does:

1. Lays out ONLY the CFlat fields as a literal (unnamed) LLVM struct type with the same field
   padding rules `CreateStructType` uses, to learn the field block size `N` and alignment `A`.
   The ForwardRefScanner never computes a layout (ruling 2026-08-27).
2. Generates C++ source and submits it as the `extraSource` of a type request under the owning
   import group (or a synthetic group when there is no base), then requests the class through
   `RequestCxxForeignType` like any imported class:

```cpp
namespace __cflat_user {
struct Foo final : public ::ns::Base {
    alignas(A) unsigned char __cflat_fields[N];
    Foo(P1 p1, ...) : ::ns::Base(<args>) { __cflat_ctor_Foo_k(this, p1, ...); }
    Foo(const Foo&) = delete;
    Foo& operator=(const Foo&) = delete;
    Foo(Foo&& o) noexcept : ::ns::Base(static_cast<::ns::Base&&>(o)) { __cflat_move_Foo(this, &o); }
    Foo& operator=(Foo&&) = delete;
    ~Foo() { __cflat_dtor_Foo(this); }            // then clang destroys Base
    R m(P...) override { return __cflat_ovr_Foo_m(this, P...); }
};
}
extern "C" void __cflat_ctor_Foo_k(__cflat_user::Foo*, P1, ...);
extern "C" void __cflat_move_Foo(__cflat_user::Foo*, __cflat_user::Foo*);
extern "C" void __cflat_dtor_Foo(__cflat_user::Foo*);
extern "C" R __cflat_ovr_Foo_m(__cflat_user::Foo*, P...);
```

   C++ class name: `__cflat_user::` + CFlat name with `.` replaced by `__`. The move
   constructor is emitted only when the base is copy- or move-constructible (extracted
   structor data); otherwise `Foo(Foo&&) = delete;`. With no base the `: Base(...)` parts are
   omitted. `final` is always emitted (no further derivation in v1).
3. After the request registers Foo with clang's flattened layout (vptr slot, base storage,
   `__cflat_fields` as a byte field), CFlat SPLICES its own fields in place of the byte field,
   inserting padding so each field sits at `offset(__cflat_fields) + literal offset`. The
   result stays a flat, index-addressable field list, so every `CreateStructGEP` site, `sizeof`,
   the destructor field walk and the move dataflow work unchanged. `VerifyCxxRecordLayout`
   must still pass; a spliced field whose DataLayout offset differs from the expected one is an
   internal error (LogError), never silent.
4. CFlat emits the thunks as ordinary LLVM functions with external linkage and the exact
   names above:
   - `__cflat_ctor_Foo_k`: seeds the CFlat field defaults (`= default` / initializers), then
     runs the user constructor body with `this` bound to the pointer parameter. A struct
     without a user constructor gets `__cflat_ctor_Foo_0(this)` for the defaults. A user
     constructor no longer returns the struct by value; construction is always in place
     (`Foo(args)` lowers through the existing C++ structor path with the slot as `this`).
   - `__cflat_dtor_Foo`: user `~Foo` body (if any), then the CFlat field teardown walk over
     the spliced fields only (never the base part, never the vptr).
   - `__cflat_move_Foo`: memcpy of the field block from source to destination, then zero the
     source block. Field destructors on a zeroed block must be no-ops for owning pointers and
     core owning types; a nested struct with a user destructor runs it on the zeroed state,
     exactly as C++ runs destructors on moved-from members.
   - `__cflat_ovr_Foo_m`: the reverse-ABI thunk for the CFlat method `Foo.m`
     (`GetOrCreateReverseAbiFunctionThunk`) with external linkage and the stable name; the ABI
     plan comes from the base virtual's extracted signature.
   The stub gets `extern "C"` DECLARATIONS of these (new: today every `extern "C"` in generated
   source is a definition). `LinkCxxCompanionModules` internalizes companion functions and runs
   GlobalDCE; the derived class's vtable is data and survives, the CFlat-side thunks are
   external and must not be internalized or dropped.
5. Destruction at CFlat scope exit uses the existing `GetOrCreateCxxClassDestructor` binding of
   the complete-object `Foo::~Foo`, so scope-exit, break, return and `delete` paths are
   unchanged. The "C++ record: bound dtor only, no member walk" guard prevents double teardown.
6. Calls: `foo.m()` on an override calls the CFlat method directly (Foo is final). Base
   members reach through the existing inherited-member registration; `protected` members of
   the base are accessible inside Foo's own methods. Virtual dispatch from C++ through a
   `Base*` reaches the clang-emitted `Foo::m`, which forwards to the thunk.

## Diagnostics (all `LogError`, all with a test)

| Case | Message (substring pinned by the err test) |
|------|--------------------------------------------|
| `override` names no base method | `is marked override but base 'X' has no virtual method named 'm'` |
| `override` on a non-virtual base method | `is marked override but 'X.m' is not virtual` |
| `override` signature mismatch (clang refuses) | `does not override 'X.m'` + clang text |
| base virtual hidden without `override` | `hides virtual method 'X.m'; add override` |
| pure virtual left unimplemented | `does not override pure virtual method 'X.m'` |
| `virtual` written | `'virtual' is not a CFlat keyword; virtual-ness is deduced from the C++ base` |
| `override` outside a `[cpp] struct` with a base | `'override' is only valid in a [cpp] struct with a C++ base` |
| base method is `final` | `overrides 'X.m' which is final` |
| base is not a C++ class | `base 'X' of struct 'Foo' is not a C++ class` |
| two bases | `multiple bases are not supported yet` |
| virtual-base base | existing `uses virtual inheritance, which is not supported yet` |
| generic `[cpp] struct` | `generic [cpp] struct is not supported yet` |
| implicit copy | existing deleted-copy diagnostic |
| use after move | existing use-after-move diagnostic |
| base initializer on a plain struct | `base initializer is only valid in a [cpp] struct` |
| protected member from outside | existing private/protected access diagnostic |

## Test matrix

Fixture rows live in `Test/test_cpp_interop.cb` (new sections M73 = 1360-1399 run A, M74 =
1400-1439 run B), fixture C++ in `Test/library/cpp_interop_poly.h/.cpp` (extended, no new
fixture files), negative tests one per rule in `Test/errors/err_cpp_struct_*.cb`. Every row
asserts a VALUE or a RESOURCE COUNT that differs between right and wrong behaviour; a row that
cannot fail is a defect (lessons file). Build the corpus first and record pre-fix behaviour.

Run A, no base (M73):

| Row | Axis | Assertion |
|-----|------|-----------|
| 1360-1362 | fields, defaults, ctor param | `Plain p = Plain(5)`: a==5, name default, second field default |
| 1363 | method | `p.total()` reads fields through `this` |
| 1364 | field write | `p.a = 9; p.total()==18` |
| 1365-1366 | sizeof/alignof | equal clang's (a `cpp_sizeof_plain()` helper is NOT possible since the class is CFlat-generated; assert `sizeof(Plain) == 8 + 16` style exact values derived from field types and alignment) |
| 1367-1368 | user dtor | counter == 1 after scope, == 0 inside |
| 1369-1370 | owning field freed once | `unique`-pointer field to a counted CFlat struct: freed_once == 1 after scope |
| 1371-1373 | explicit move | `Plain q = move p;` q.a==5, dtor counter total 2 (both C++ dtors run), pointee freed once |
| 1374 | return by value from a CFlat function | `Plain make(int)` returns a moved value; caller sees a |
| 1375 | pointer param | `void bump(Plain* p)` mutates through pointer |
| 1376-1377 | new/delete | `Plain* h = new Plain(2); delete h;` dtor counter +1, pointee freed |
| 1378 | static method | `Plain.make_tag() == 7` |
| 1379 | namespaced `[cpp] struct` | `ns.Q q = ns.Q(); q.v == 3` |
| 1380-1381 | `[cpp] struct` as field of a plain struct | outer scope exit runs inner C++ dtor once |
| 1382-1383 | `[cpp] struct` field inside another `[cpp] struct` | inner dtor runs once via the field walk |
| 1384 | `[cpp]` explicit + no base, with `alignas`-free i64 field | offset check via a method returning `&b` difference |
| 1385 | warm cache | (verification step, not a row) rebuild fixture twice, second compile rc 0 and identical output |

Run B, base + override (M74) - see the parent plan's M10 entry once run A lands; the axes:
override value through CFlat call, through a C++ `Base*` caller in the fixture
(`call_area(const Shape*)`), virtual delete from C++ (`destroy(Shape*)`) running the CFlat dtor,
base ctor args (`Tagged(int)` fixture class), default base ctor, base field read/write, base
non-virtual method, override of a `const` method, pure-virtual implementation (`Abstract`),
two-level base (`Circle`), protected member inside/outside, `move` of a derived value, passing
to a `const Shape&` parameter, `double` and pointer parameters through the override thunk,
and every diagnostic in the table above.

## Caches

- Extractor gains `isOverride` / `isFinal` per member; header cache codec keys `ov` / `fi`,
  version 55 -> 56 with a history line.
- The type-request cache key must include a hash of the generated class source (extraSource);
  a changed field list must never hit a stale entry.
- Any new `StructData` field an analysis reads round-trips in the `--init` serializer.

## Not in v1

Multiple bases, virtual bases, new virtuals visible to C++, `base.m()` qualified calls to the
overridden base implementation, protected base members reached through a derived-typed pointer
other than `this` (C++ allows it; the gate keys on `this`), generic `[cpp] struct` (per-instantiation
generated classes; p2 issue internal/issue/p2/cpp-struct-generic-instantiations.md), `[cpp] struct`
by value inside a std container (p3 issue), `unique_ptr` vs `unique<T>` mapping,
MSVC verification.

## Landed after v1 (same day, working tree)

- Template bases (run F): `struct T : tb.Holder<int>` with `: tb.Holder<int>(v)` initializer;
  mismatch diagnostic `base initializer names '{}' but the base class is '{}'`; M76 (1460-1479),
  err files tpl_base_init_mismatch, tpl_base_generic_param, tpl_hides_virtual,
  tpl_pure_not_overridden, base_init_mismatch_plain.
- nn::Module spike (run G), both defects general interop, not `[cpp]`-specific: returned C++
  temporaries are adopted by the caller instead of destructed before `ret`
  (`UnregisterOwnedStructTemp` on the return path); implicit-`this` calls to inherited C++
  member templates (`register_module(...)`) resolve. M77 (1480-1499),
  err_cpp_struct_member_tpl_no_this. `register_module` therefore works in v1 with a
  `std.shared_ptr<torch.nn.LinearImpl>` field; `std::make_shared<Net>` of a `[cpp] struct`
  remains timebox 2.
- Timebox 2 (2026-09-14, runs H and I, 7 h): `std.shared_ptr<Leaf>` / `std.make_shared<Leaf>(...)`
  of a `[cpp] struct`. Class-template type requests now inject the generated class text
  (`GeneratedCxxDefinitionsFor`, through `prefixSource` so the disk-cache key covers it) with
  the base's header groups; a struct used as a template argument before its definition gets a
  forward declaration (tentative request) and is promoted when the definition appears, a
  template needing the complete type (`std.vector<Leaf>`, `std.deque<Leaf>`) before the
  definition reports `'{}' needs the complete definition of [cpp] struct '{}'; define the
  struct before this use`. Member templates with several same-arity overloads keep every
  overload (torch's `register_module(string, shared_ptr<T>)` / `(string, ModuleHolder<T>)`),
  and instantiations are keyed by owner. Upcast `shared_ptr<Leaf>` -> `shared_ptr<Base>` by
  declaration and at calls; virtual dispatch from C++ through the base handle reaches the
  CFlat override; make_shared runs the generated ctor and `~Leaf` runs the CFlat dtor.
  A forward-declared (tentative) specialization is promoted by re-registering its whole
  instance member set once the definition exists (`get()` / `operator*` on a handle whose
  argument class is declared later). Fixture M78 (1500-1553), err files tpl_arg_incomplete (+deque), ladder rung t30 (custom
  Block sub-module via make_shared, Net via make_shared, upcast to shared_ptr<Module>,
  children()==2, trains). Known: `std.vector<Leaf>` BY VALUE fails at the request
  (internal/issue/p3/cpp-struct-by-value-in-std-container.md); copying a nontrivial C++ class
  value out of a FIELD skips the copy constructor and the copy is later treated as owning
  (internal/issue/p2/cpp-class-copy-from-field-skips-copy-ctor.md, general interop, needs a
  ruling); the fixture cold compile is ~120 s
  (internal/issue/p2/cpp-interop-fixture-near-timeout.md, test.sh timeout 240).
