# C++-class fields of CFlat structs: default-construction and initializer leftovers after V20

3. (p3) A brace field default on a struct without a user ctor loses the C++ field's value:
   `struct W { H h = { d = cnt.Del(5) }; };` then `W w;` gives d.v = 0 (d/p.cb). With a user ctor it
   is correct.
4. (p3) A field initializer that names an earlier field is "Undefined variable" (C++ allows it).
   Rule needed before fixing.
