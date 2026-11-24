# C++ `const T*` result stored into a mutable same-class pointer local is accepted

Found by PTRSAFE (2026-11-19). clang++ -std=c++20 refuses every case below; CFlat accepts them
and lets the local inherit the pointee const (writes through it are refused, so no write hole
was found). Native CFlat refuses the same store (`NS* p = gc(&x)` with `const NS* gc(NS*)`:
"the conversion discards const"). PTRSAFE refuses the BASE-class form (`F* q = getcd(&d2)`,
`const D2*` into `F*`) at every store position and argument; the same-class forms stay open
because existing legs pin them as accepted:

- Test/test_cpp_interop_template.cb:3637 `nest.Cell *crpMemberConstReturn = crpHolder.const_out();`
- Test/test_cpp_interop.cb leg 7954 `mtConstPass(mtcH.cpv())` (C++ const result into a CFlat
  `MtConstObj*` parameter: the comment says CFlat parameters have no const spelling).

## Repro (header rr.h)

```cpp
struct Pad { int pad = 11; virtual ~Pad() = default; };
struct B0 { int b = 2; };
struct F : B0 { int value = 3; };
struct D2 : Pad, F { int z = 1; };
inline const D2* getcd(D2* p) { return p; }
inline const F* getcf(F* p) { return p; }
```

```cflat
import cpp {"rr.h"};
int wf(F* p) { p->value = 1; return 2; }
int main(int argc, char** argv) {
    bool c = argc > 0; F a; D2 d2; d2.value = 5; F* fp = &a;
    D2* q1 = getcd(&d2);            // CFlat 1 (accepted)   clang: error
    F* q2 = c ? getcd(&d2) : fp;    // CFlat 5 (accepted; the join is `const F*`)   clang: error
    return wf(getcf(&a));           // CFlat 2: wf WRITES through a const F   clang: error
}
```

The last line is a real write through a C++ const object (same class, direct call result into
a CFlat callee); `F* r = getcf(&a); wf(r)` is refused today, the direct form is not.

## Fix direction

Needs a ruling: either match the native policy (refuse the store; update the two pinned legs) or
keep the inherit-const local and refuse only the direct C++ const result into a CFlat parameter
(ConversionDiscardsConst: count the C++ result's type-carried pointee const at argument sites,
LLVMBackend_Overloads.cpp ConversionDiscardsConst callers; leg 7954 then changes).

## Related, older (PTRSAFE review r1)

- `std.stringstream ss; std.ostream* o = &ss;` is refused as "not a PUBLIC base" (virtual-base route
  through basic_ios); clang accepts. Explicit casts there are right since PTRSAFE.
