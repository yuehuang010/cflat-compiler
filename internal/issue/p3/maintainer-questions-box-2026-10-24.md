# Maintainer questions from T442 and T449 reviews

Summary: Two review findings need a behavior ruling.

- T442 r1: Should untyped `return { x = 1 }` be supported for movable structs? Options: support it like immovable CFlat structs, or keep it restricted and document the rule. Master currently refuses it.
- T449 r1: Should `size_t strlen` results implicitly narrow at calls and function-pointer initialization? Options: permit C-style narrowing at those sites, or keep explicit narrowing required. `takeInt(strlen(s))` and `function<int((const char)*)> f = strlen` are refused, while `int n = strlen(s)` compiles; signed/unsigned comparison follows C.

Found: T442 r1 and T449 r1 reviews, 2026-10-24 box, PRE-EXISTING
