Bucket: p2 (C++ bridge; ruling R5, 2026-09-23)

# The `unique` keyword applies std::unique_ptr<T> to a C++ class type and unique<T> to a CFlat type

## Ruling (maintainer, 2026-09-23)

`std::unique_ptr<T>` and `unique<T>` stay two different types. The `unique` KEYWORD is what
bridges them: `unique T*` / `unique<T>` with T an imported C++ class means `std::unique_ptr<T>`;
with T a CFlat native type it means the library `unique<T>` as today.

## Clarification (maintainer, 2026-09-23, later): `unique` and `move` are SUGAR keywords

On a C++ type the two keywords ARE the C++ facilities, not CFlat look-alikes: `unique T*` /
`unique<T>` is `std::unique_ptr<T>`, and `move x` is `std::move(x)`. No CFlat-side ownership
model runs for a C++ pointee; the C++ type's own special members do all the work. On a CFlat
native type they keep meaning the library `unique<T>` and CFlat's destructive move.

## Interpretation (main session reading of the ruling)

- `unique cpp.Widget* w = new cpp.Widget(1);` holds a `std::unique_ptr<cpp.Widget>`: same storage,
  same destructor path (the class's deleter), scope-exit and move semantics as the C++ type.
- A C++ API taking `std::unique_ptr<T>` by value / `&&` accepts `move w`; one returning
  `std::unique_ptr<T>` adopts into `unique T*` with no conversion call.
- `[cpp] struct` (CFlat-defined, C++-shaped) follows the C++ side.
- Interfaces / drained-state rules of `unique<T>` do not apply to the C++ side; C++ rules do.

## Repro of the gap today

`unique cpp.Widget* w = new cpp.Widget(1);` lowers through unique<T> (core unique.cb), so
passing `move w` to `void take(std::unique_ptr<Widget>)` is refused, and a
`std::unique_ptr<Widget> make()` result cannot be declared as `unique Widget*`.

## Fix direction

Type-check at the `unique` declaration: C++ pointee -> request `std::unique_ptr<T>` as the
slot type (the specialization request path in cflat/LLVMBackend_CInterop.cpp), and route
`new T(args)` adoption, `move`, scope exit and `->` through the C++ type. Spike the ABI of
`std::unique_ptr` by value at calls first (it is non-trivial for the purposes of calls on
Itanium: passed indirectly). Plan record: internal/plan/cpp-bridge-transparency.md R5.
