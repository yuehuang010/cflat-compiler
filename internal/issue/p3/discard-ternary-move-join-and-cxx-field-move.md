# Discard sink leftovers after T352: raw owning field discard, converting-ctor gap

Severity p3. Found by the T352 review (2026-10-20). The native `?:` move-join leaks (MIX19 residue: mixed
string joins, nested all-move joins, string assignment from a join, struct-with-dtor `_ =` joins, bare
`c ? move a : move b;` statements) were fixed in STRJOIN; legs Test/test_move.cb testStrjoinTernaryMoveJoin.
The C++ `_ = move p->c` item was fixed in TERNREST (3c95a46e2).

3. Not fixed in MIX19 (by design, not a fix candidate). `_ = move o.raw;` where the user dtor deletes the raw field leaks.
   Root cause (MIX19): a raw `Val*` owns nothing (raw lifetimes are untracked, memory-safety-function-boundaries-only ruling).
   The move nulls the field, so ~O deletes nothing, and discarding a non-owning pointer has no drop. The owning spelling
   `Val* p = move o.raw; delete p;` is leak-free.
   Clang twin: a raw pointer move is a copy with no delete, so clang also frees nothing here.
   Repro:
   ```cflat
   struct Val { int v = 0; };
   struct O { Val* raw = default; ~O() { delete raw; } };
   void f() { O o = default; o.raw = new Val(); _ = move o.raw; }   // leaks the Val (by design)
   ```

7. Converting-constructor gap (pre-existing on master): `A a = mkB();` is refused when A has a converting ctor A(B),
   although the converting-constructor ruling (string-literal-cpp-class-ruling.md) says it should convert. Master
   reports the decay text; MIX19 reports "cannot initialize or assign 'A' from unrelated struct type 'B'". Not MIX19's fix.
   Repro:
   ```cflat
   struct B { int v = 1; };
   struct A { int v = 0; A(B b) { v = b.v; } };
   B mkB() { B b = default; return b; }
   int main() { A a = mkB(); return a.v; }   // expected 1; refused
   ```
