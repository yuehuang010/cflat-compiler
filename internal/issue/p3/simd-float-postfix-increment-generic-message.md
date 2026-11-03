# simd<float, 4>++ is refused with a generic lane-count message

T71 review 1 (2026-10-07); master same. `simd<float,4> v; v++;` -> "simd operands must have the same
lane count and element type" - misleading (the operand is fine; the implicit 1 is a scalar). Decide:
lane-wise increment (C++ vector extensions accept `v++` on GCC vector types) or a specific refusal.
RULED 2026-10-31 (main session, from precedent; maintainer confirmed 2026-10-31): LANE-WISE increment - `v++` / `v--` on `simd<T, N>` means
`v = v + 1` with the scalar splat CFlat already applies to simd arithmetic (return-follows-assignment ruling 2026-09-04, splat
included); GCC/clang vector extensions accept `v++` the same way. Test consequence: `simd<float,4> v; v++;` becomes an accept leg
(each lane +1); the misleading lane-count message disappears for this shape.
Prior art check 2026-10-31 (Haiku research, maintainer asked): C++26 std::simd ([simd.unary], eel.is/c++draft/simd.unary) has member
prefix/postfix ++ and -- on basic_vec (earlier drafts: basic_simd), noexcept, "increments/decrements every element by one"; the only
constraint is `requires (value_type a) { ++a; }`, so FLOAT lanes qualify - the lane-wise ruling above matches the C++26 parent, not only
the GCC/clang vector extensions. Note for the splat: std::simd's implicit broadcast ctor ([simd.ctor]) requires a value-preserving
conversion, so `simd<float> + 1.0` (double literal) is ill-formed there; CFlat's `1.0` is float by design (DEFERRED 2026-10-13), so `v + 1.0`
splats without that question arising. Mask types: ++ not established; out of scope (CFlat postfix-only rule stands).
