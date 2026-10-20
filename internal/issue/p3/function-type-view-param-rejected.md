# Function types cannot spell a view parameter

## Summary
`function<int(int[])>` (and `Lambda<>` / fn-pointer forms) fail to parse: `found '<' but expected {'move', '(', Identifier}`.
`functionPointerParam` (CFlat.g4 ~:401) has no `arrayTypeSuffix?`, so no function type can take `T[]` or `(const T)[]`.

## Repro
```cflat
int sum(int[] v) { return 0; }
extern int main() { function<int(int[])> f = sum; return f(nullptr); }
```
Master (74834c205) refuses at `<`. C++20 accepts the analogue `std::function<int(std::span<const int>)>`.

## Fix direction
Add `arrayTypeSuffix?` to `functionPointerParam`; carry view + element const through both function-type parse paths
(phase-7 marker, c14 FuncPtrParam::ConstMask), exact-match identity per the 2026-10-19 function-type ruling. Found by
the c16 Sol review.
