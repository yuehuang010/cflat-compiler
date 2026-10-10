# C++ class temporaries passed into C++ (by-value args, brace elements) cost an extra move + dtor

Found by T63 review 2 (reclassified pre-existing family by the main session: master does the same at
C++ by-value call sites). Values are correct, nothing leaks; counts are observable.

## Repro (scratch/repro_keep/t63_rev/r2/)
```cflat
std.vector<pr.C> v{pr.C(30), pr.C(40)};   // clang++ -std=c++20: 2 ctor, 2 copy, 0 move, 2 dtor
                                          // cflat (T63): 2 ctor, 2 copy, 2 move, 4 dtor
pr.byVal(pr.C(3));                        // clang: 0 move; cflat master: 1 move
```
CFlat materializes the temporary in a CFlat slot, then the generated wrapper moves it into the C++
parameter / initializer_list backing array. C++17 guaranteed elision constructs the prvalue
directly in place.

## Fix direction
For a prvalue class-ctor argument/element, emit the construction inside the generated wrapper
(pass the ctor arguments through) instead of materializing first; T63's brace thunk and the
by-value call wrapper share the need. Leg 9306 (test_cpp_interop.cb) deliberately does not assert
moves - tighten it when fixed.
