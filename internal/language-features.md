---
name: language-features
description: CFlat language feature reference - generics, interfaces, arrays, ownership/move, compile-time features
metadata:
  type: reference
---

# CFlat Language Features

For the full user-facing language reference see `doc/LANGUAGE.md`.

## Feature List

- **Generics**: `struct Box<T> { T value = default; }` - monomorphized at compile time
- **Interfaces**: `interface IReadable { int Read(); }` with VTable dispatch; fat pointer layout `{i8* vtable, i8* data}`
- **Namespaces**: `namespace Math { ... }` with qualified access `Math.square()`; dotted form `namespace os.windows { ... }` declares the nested chain. An `extern` inside a namespace registers under the qualified lookup name but keeps its bare linkage symbol - core's `os.windows.Sleep` and a header-imported bare `Sleep` bind to the same imported function. Core's Win32/CRT bindings all live in `os.windows`, so `import "windows.h"` registers its structs (CONSOLE_SCREEN_BUFFER_INFO, INPUT_RECORD, ...) unshadowed.
- **Type aliases**: `using M = Math` - expands at reference time
- **Module system**: `import "file.cb"` for multi-file compilation; `import { "a.cb", "b.cb" };` is a grouped shorthand for several plain imports on one line (each entry routes exactly like a bare `import "x";` - same `.cb`/`.h`/`.c` dispatch, no per-entry `lib`/`define`). A trailing `cache` on a group applies to **every** entry (`import { "a.h", "b.h" } cache;` caches both - a no-op for `.cb`/`.c` entries). `as` is meaningful only on a single-entry group (one alias cannot name many files).
- **Sized integers**: `i8, i16, i32, i64, u8, u16, u32, u64`; C aliases map to fixed widths
- **Implicit upcast, no implicit downcast**: widening conversions (e.g. `i32` -> `i64`, derived -> base/interface) happen implicitly; narrowing conversions (downcasts) require an explicit cast
- **Null-safe access**: `ptr?.field` - works on any struct pointer (local variables, function arguments, struct member fields)
- **Null-coalescing**: `a ?? b` (zero-check for integers)
- **Ternary operator**: `condition ? trueValue : falseValue` - compact conditional expression; both branches must be compatible types
- **`break` / `continue`**: exit or advance the nearest enclosing `while`, `for`, `foreach` loop
- **Operator overloading**: `operator+`, `operator==`, `operator new`, `operator delete`
- **Function overloads** with type-based resolution and default parameters
- **Named parameters**: `func(x: 1, y: 2)` - args matched by name, any order
- **Return-block functions**: `return { ... }` - body inlined at call site
- **Intrinsics**: `typeof()`, `nameof()`, `sizeof()`, `alignof()`
- **Range-based for**: `foreach (T x in collection)` - calls `count()` / `get(int)` on the collection
- **Program**: `program Name { fields...; int main(move list<string> args) { ... } };` - struct-like construct with a managed entry point; instantiate with `Name p;`, configure fields, then call `p.run(args)`. The compiler auto-generates `run()`, which spawns a dedicated thread, installs a per-thread `MallocAllocator` by default (override by setting `p._allocator` before `p.run()`), calls `main`, joins the thread, and returns the exit code. Requires `import "list.cb"` and `import "thread.cb"`.
- **Fixed-size arrays**: Use type-first syntax `T[N] name` - the size belongs to the type, not the name. C-style `T name[N]` is a compiler error for non-pointer types. Initialization forms:
  - `T[N] name;` - declared, not initialized
  - `T[N] name = default;` - zero-initialized
  - `T[N] name {}` or `T[N] name = {}` - value-initializes all N elements by calling the default constructor (struct types only)
  - `T[N] name {field=v, ...}` - seeded value-init; overrides the listed fields in each element
  - `T[N] name = {v0, v1, ...}` - **positional** element list; fewer than N zero-fills the trailing slots, more than N is an error (`EmitPositionalFixedArrayInit` in `MainListener.h`)
  - Struct fields still use C-style `T name[N]` (grammar limitation at struct scope)
  - Pointer arrays use `T*[N] name` (type-first); C-style `T* name[N]` is still accepted (exempt from the C-style error since `T*[N]` was not always available)
- **Length-inferred array-view**: `T[] name = {v0, v1, ...}` infers N from the element count. `T[]` is a thin `T*` (IsArrayView), so the brace list both allocates an inline `[N x T]` backing array and points the `T*` at element 0; N is compile-time only. Empty `{}` is an error (`EmitArrayViewInferredInit` in `MainListener.h`).
- **Container brace init**: `list<T> x = {a, b}` (-> `add()`), `array<T> x = {a, b}` (-> `init(N)`+`set()`), `dictionary<K,V> x = {k: v}` (-> `set()`). char* literals coerce to `string`; otherwise elements follow normal conversion rules (widening implicit, **no implicit down-convert** - `list<float> = {1.5}` is rejected). Empty `{}` yields an empty container (`TryEmitContainerInitializer` in `MainListener.h`). All brace-init paths above are **local-scope only**; globals still error.

## Ownership / Lifetime (`move` keyword)

CFlat uses **context-based ownership**: local variables own the pointers they allocate; function parameters borrow by default; struct fields own nothing unless marked `unique`. `unique` is also legal on locals, parameters, and generic type arguments (`list<unique T*>`) - see `doc/LANGUAGE.md`'s "`unique` Ownership" section for the full rules.

The `move` keyword on a **parameter definition** transfers ownership into the callee:

```cpp
void consume(move Resource* r) { ... }  // r is freed when consume() returns
void borrow(Resource* r)        { ... }  // caller still owns r
```

- **Call site is silent** - no annotation needed; the caller's pointer is automatically nulled after a `move` call.
- `move` is a **soft keyword** - detected via text matching in `ParseDeclarationSpecifiers()`, not as an ANTLR lexer token (avoids breaking identifiers/methods named `move`).
- For primitive value types (int, float, bool, etc.) `move` is a no-op - ownership semantics only activate when `TypeAndValue.Pointer == true` **or** the value is an owning `string`. `string` owns a heap buffer (`_ptr` field) and is zeroed on move even though it is not a pointer type; use-after-move of `string` is a compile error like any pointer type.
- `list<T>::add(move T value)` and `dictionary<K,V>::add/set(K, move V value)` take a `move T`/`move V` argument regardless of ownership - a NAMED pointer local passed in is always nulled by the call, even when the target does not end up owning it (see the `list<T*>` note below).
- **`list<T>` element ownership now keys off `is_unique(T)`, not `is_pointer(T)` (BREAKING CHANGE).** A bare `list<T*>` / `list<IShape>` is a **borrowed view** - `add`/`set`/`removeAt`/`clear`/the destructor never free pointer or interface elements. Only `list<unique T*>` / `list<unique IShape>` **owns** its elements: the destructor, `removeAt()`, `clear()`, and overwriting `set()` free them (interface elements via the vtable dtor slot); `take(i)` transfers ownership out; `[]`/`get(i)` return a borrow (`alias`); `.copy()` on a unique-element list is a compile error ("cannot copy a list of unique elements; use move or copy elements explicitly") - a bitwise copy would double-free. `list<unique T*>` and `list<T*>` are distinct instantiations with no implicit conversion. Code written before this change that relied on bare `list<T*>` owning its elements by default must add `unique` to the element type or it silently stops freeing them.
- **ASYMMETRY (migration pending): `dictionary<K,V>` and `hashset<T>` have NOT been migrated to `is_unique`.** They still key purely off `is_pointer(K)`/`is_pointer(V)` and own bare pointer keys/values exactly as before this change - `unique` has no effect on their ownership behavior yet. Only `list<T>` follows the new `is_unique`-based rule. Do not assume dictionary/hashset pointer semantics match list's; check the source (`cflat/core/dictionary.cb`, `cflat/core/hashset.cb`) before relying on either.
- **Pitfall:** because `add`/`set` always take `move T`, adding a *named* owning pointer local to a *borrowed* `list<T*>` still nulls the source variable - but since the borrowed list never frees it, the object is now leaked (owned by nobody). Feed borrowed lists from rvalue borrows (e.g. `owner[i]`) instead of named owning locals. A design refinement (a non-move `add` overload, or a diagnostic) is under consideration but not implemented; this is documented current behavior, not a bug to route around silently.

Implementation: `EmitOwningPtrCleanup()` in `LLVMBackend.h`; `EmitDestructorsForScope()` handles move-param cleanup at scope exit; `CreateOverloadedFunctionCall()` nulls the caller's storage after the call.

## Compile-Time Features

- **`if const`**: Condition evaluated at compile-time; dead branches eliminated from IR. Works at both function and file scope.
- **Compile-time macros**: `__FILE__`, `__FUNCTION__`, `__LINE__`, `__PLATFORM__` (64 or 32), `__WIN64__` / `__WIN32__` / `__WINDOWS__`, `__X86__` (1 on the x86/Intel target; guards architecture-specific intrinsics). Available globally.
- **`is_pointer(T)`**: Compile-time intrinsic that returns 1 if the type parameter T resolves to a pointer type in the current generic instantiation, 0 otherwise. Use with `if const` to branch on element ownership in generic code.
- **`is_unique(T)`**: Compile-time intrinsic, the `unique` counterpart to `is_pointer(T)` - returns 1 if T resolves to a `unique`-qualified pointer or interface type in the current generic instantiation, 0 otherwise (0 outside a generic context, same convention as `is_pointer`). Meaningful only under `if const`.
- **`compile_error("msg")`**: Builtin usable inside `if const` in generic/library code. Marks the enclosing method as poisoned for that instantiation (deferred-poison, not fire-on-instantiation - generic methods instantiate eagerly, so the error only fires if the method is actually CALLED, via `CheckPoisonedFunctionCalls()`; a poisoned method reached only through virtual interface dispatch is not caught). Used by `list.cb` to reject `.copy()` on `list<unique T*>` / `list<unique IShape>`.
- **Platform support**: `-p win64` (default) or `-p win32`; guard with `if const (__PLATFORM__ == 64) { ... }`
