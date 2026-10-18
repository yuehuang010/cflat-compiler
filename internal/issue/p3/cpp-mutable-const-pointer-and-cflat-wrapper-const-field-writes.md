# Writes through a mutable const C++ pointer member, and to a const C++ field of a CFlat struct, are accepted

Found by the T335 round-1 review (2026-10-17); the same behaviour on master and the T335 branch. Repros (header axes.hpp) are in
scratch/repro_keep/box1017/t335_rev/.
- `struct Leaf { int x; const int k; }; struct Ptr { mutable const Leaf* p; }; inline const Leaf other{8,9}; inline const Ptr ptr{&other};`
  Then `ptr.p->x = 11;` compiles. At runtime it writes read-only storage (exit 138). clang refuses it: the pointee is const, and
  `mutable` covers only the pointer slot.
- `struct Wrap { const Leaf l = {x = 1, k = 2}; }; Wrap w; w.l.x = 11;` compiles. clang refuses the equivalent C++ write to a
  const member. CFlat drops `const` on its own declarations (const-unenforced ruling); decide whether a const-qualified C++
  class field inside a CFlat struct should be enforced like a const C++ object.
Fix direction: item 1 - writing through the loaded pointer must check the pointee's constness (the T225/T324 const-pointee
machinery). Item 2 needs a ruling against const-unenforced-ruling first.
