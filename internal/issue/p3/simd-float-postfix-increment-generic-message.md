# simd<float, 4>++ is refused with a generic lane-count message

T71 review 1 (2026-10-07); master same. `simd<float,4> v; v++;` -> "simd operands must have the same
lane count and element type" - misleading (the operand is fine; the implicit 1 is a scalar). Decide:
lane-wise increment (C++ vector extensions accept `v++` on GCC vector types) or a specific refusal.
