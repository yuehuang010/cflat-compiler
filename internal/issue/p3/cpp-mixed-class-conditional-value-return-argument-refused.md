# Mixed-class C++ conditional used as a value, return or argument is refused (clang accepts)

T513 residue (review legs v10-v12). T513 lowers a bare-statement or discarded `?:` whose arms are
different C++ classes through clang's classification. The same conditional in a value position is
still refused, while clang converts the arm to the common type (here `B(A)`) and accepts.

## Repro (types from Test/library/cpp_interop_ternaryref.h)

```cflat
import cpp "cpp_interop_ternaryref.h";
T513R8B ret(bool k) { return k ? t513R8MakeA(1) : t513R8MakeB(); }  // cannot return C++ class 'T513R8B' from this conditional expression
void take(T513R8B b) { }
extern int main()
{
    bool k = true;
    T513R8B b = k ? t513R8MakeA(1) : t513R8MakeB();  // cannot initialize C++ class 'T513R8B' from this expression
    take(k ? t513R8MakeA(2) : t513R8MakeB());         // ternary branches have incompatible types
    return 0;
}
```

## Fix direction

Reuse the T513 clang classification of the actual conditional (arm types + value category) and
produce the common-type result in place into the destination (declaration slot, sret, call
argument temp). Refusals clang gives (ambiguous, explicit conversion) stay refused. Check ctor/dtor
sequences against a clang++ oracle on both ABIs (MSVC destroys a by-value parameter in the callee).
