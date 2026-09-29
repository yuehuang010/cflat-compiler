Bucket: D (wrongly refused; C++ operator fold carry). Found 2026-09-28 by the D2 review (pre-existing on master).

# C++ operator chain whose operator returns by REFERENCE is refused when consumed as a value

`cppfop.Box<int> x = a | b | c;` (Box's `operator|` returns `Box&`; fixtures in
Test/library/cpp_interop_freeoptpl.h) -> "cannot initialize value of type 'cppfop.Box<int>' with a pointer
of type 'cppfop.Box<int>*'". clang++ -std=c++20 accepts, value 7. Same on master and after D2 (fix/op-dispatch).
Site: the fold result carry (CarryCxxOperatorResult, MainListener_Expressions.cpp ~19626 after D2): the final
reference result stays pointer-shaped when the fold is consumed as a value. Direction: when the last fold
step's C++ result is a reference and the consumer wants a value, load / copy-construct through the class's
copy ctor exactly as a single `a | b` reference result is consumed today. Check every fold kind (the D2 legs
at Test/test_cpp_interop_template.cb ~1844-1870 cover sret and scalar results, not references).
