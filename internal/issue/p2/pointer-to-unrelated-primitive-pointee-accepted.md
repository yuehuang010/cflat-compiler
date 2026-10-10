# int* converts silently to double* (unrelated primitive pointee) at a single-overload call

A non-overloaded `g(double*)` accepts an `int*` argument; clang refuses (no conversion int* -> double*). Reading
through it reinterprets memory. With 2+ overloads T36 (parked branch fix/t36-ptrarith) already stops floating
pointees binding unrelated pointees; the single-candidate path is unchanged on master. Found by T36 round 2
(2026-10-05).

## Repro

```
void g(double* p) { printf("%f\n", *p); }
extern int main() { int x = 1; g(&x); return 0; }   // compiles, prints garbage; clang: no matching function
```

## Fix direction

Same safety ground as the 2026-09-30 integer-pointee ruling (p2/integer-pointee-pointer-conversion-accepted):
refuse, explicit cast is the workaround. Confirm the ruling extends to integer <-> floating pointees before
building; sweep assignment / init / return / ternary arms too (the integer-pointee issue lists the sites).
