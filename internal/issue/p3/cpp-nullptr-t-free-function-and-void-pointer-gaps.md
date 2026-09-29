# std::nullptr_t parameters: free-function selection and void* acceptance gaps

Found 2026-09-28 by run D1 (ctor path fixed there; these are the leftovers).
1. Free C++ functions: `fp(std::nullptr_t)` + `fp(int*)` called as `fp(nullptr)` silently picks `int*`
   (C++ picks the nullptr_t overload); a lone `fn(std::nullptr_t)` / `fo(std::nullptr_t)` refuses `nullptr`.
   Several sites (free-function selection, not SelectCxxConstructor).
2. A `std::nullptr_t` ctor parameter accepts a `void*` VARIABLE; C++ refuses (only a null pointer constant
   converts). Needs a rejection (full mode, accept-set first).
Root: `std::nullptr_t` maps to `void*`; the parameter SPELLING is the only discriminator (see D1's
SelectCxxConstructor change for the pattern). Probes: scratch/repro_keep/d1 (main checkout) if kept.
3. Implicit conversion through a non-explicit `nullptr_t` constructor is refused (D1 review, same on master):
   `rv.Im v = nullptr;` (clang: 7) gives a misleading "cannot initialize ... with a pointer of type 'rv.Im*' ...
   or drop the 'new'", and `takeIm(nullptr)` into a by-value `Im` param (clang: 7) gives "no overload matches".
   Related to the converting-constructor ruling (2026-09-26: implicit converting ctors for C++ non-explicit).
