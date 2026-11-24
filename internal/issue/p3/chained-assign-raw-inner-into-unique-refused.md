Bucket: p3 (usable-surface gap) + one silent leak (see item 2)

# Chained assignment - remaining parameter ownership gap

Still open:

2. Parameter destination leak (master has the same behavior):
   `void f(PS* raw, bool c) { PS* other = new PS(); raw = (c ? new PS() : other); }`
   With c true this frees `other` but not the fresh block (2 constructs, 1 destruct). The plain
   `void f(PS* raw) { raw = new PS(); }` also leaks (1 construct, 0 destructs). This is not chain-specific:
   argument slots carry no owner bit, and the statement store excludes stack arguments from adoption
   (`FindStackArgumentByStorage`), so there is nothing to adopt the fresh block into. Fixing it needs
   an owner bit on argument slots, or a refusal of a fresh-object store into a borrowed parameter.
   This remains a design call and is out of scope for the chain fix.
   Same family, parameter middle: `unique PS* a = nullptr; PS* e = nullptr; a = prm = e = new PS();`
   is refused as a chain (`prm` is a borrowed parameter), while `e = new PS(); prm = e; a = prm;` is
   accepted (constructs 2, destructs 2). The chain stays refused until argument slots carry an owner bit.
   Same family, raw field / global middle (split form leaks on master and the branch): with c true,
   `PS* outer = nullptr; h.raw = (c ? new PS() : nullptr); outer = h.raw;` (h a local struct with
   `PS* raw`) and `g = (c ? new PS() : nullptr); outer = g;` (g a global `PS*`) each leave 1 construct,
   0 destructs. The chain forms `outer = h.raw = (...)` and `outer = g = (...)` are balanced (1 construct,
   1 destruct): the chain hands the fresh object to the raw outer, the split copy does not. Minimal repro:
   ```
   int constructs = 0; int destructs = 0;
   struct PS { int value = 7; PS() { constructs = constructs + 1; } ~PS() { destructs = destructs + 1; } };
   struct Holder { PS* raw = nullptr; };
   PS* g = nullptr;
   int main(int argc, char** argv)
   {
       bool c = argc > 1;
       { Holder h; PS* outer = nullptr; h.raw = (c ? new PS() : nullptr); outer = h.raw; }
       printf("field constructs=%d destructs=%d\n", constructs, destructs);   // run with an argument: 1 / 0
       { PS* outer = nullptr; g = (c ? new PS() : nullptr); outer = g; }
       g = nullptr;
       printf("global constructs=%d destructs=%d\n", constructs, destructs); // 2 / 0 after both blocks
       return 0;
   }
   ```
   Owner bit on raw field/global slots is the same design call as above; not fixed by the chain work.

   OPEN 2026-11-18 (put to the maintainer, not ruled): first reply "destructor of ptr should check if not null", then
   "need more information". The crux given back: non-null is not owned - a raw field can hold a fresh object
   (`h.raw = new PS()`) or a borrow (`h.raw = &other`), so a destructor that deletes on non-null double frees the
   borrow. Candidates for fields / globals: a runtime owned flag beside the pointer; a compile-time rule that a
   field which ever receives a fresh object is owning everywhere (that is `unique`); C manual delete. A borrowed
   parameter receiving a fresh object always leaks (no caller can see it). The main session's pick, not accepted:
   refuse the fresh store into a parameter, fields and globals stay manual.

   MEASURED 2026-11-18 (master, Release, --run; PS counts constructs / destructs as in the repro above):
   ```
   { PS* a = new PS(); }                                        1 / 1   local adopts
   { PS x; PS* a = &x; a = new PS(); }                          2 / 2   local that began as a borrow adopts
   { Holder h; h.raw = new PS(); }                              1 / 0   field: nobody frees
   { PS* a = new PS(); Holder h; h.raw = a; }                   1 / 0   local gives the object up, field never frees
   { Holder h; h.raw = new PS(); PS* p = move h.raw; delete p; } 1 / 1  manual release (the spelling master asks for)
   struct HolderD { PS* raw; ~HolderD() { if (raw != nullptr) { delete raw; } } }  1 / 1  user destructor
   { g = new PS(); }                                            1 / 0   global: nobody frees
   void prm(PS* raw) { raw = new PS(); }  called with &x        2 / 1   parameter: fresh object leaks
   void prmdel(PS* raw) { raw = new PS(); delete raw; }         2 / 2   manual delete on the parameter works
   ```
   `delete h.raw;` from outside Holder is already refused: "cannot delete field 'Holder.raw' from outside
   'Holder'. Extract it first with 'T* p = move expr; delete p;' so the ownership transfer is explicit."

   PROPOSED 2026-11-18 (main session, PARENT MEANING + COMPILER SAFETY; replaces the earlier pick): (a) a pointer
   parameter is a local that starts as a borrow, so a fresh object stored into it is adopted and released at
   function exit exactly as the local in row 2 - no refusal, because row 9 is a valid program; `prm` prints 2 / 2.
   (b) raw fields and globals stay manual as C++: no owner flag, no implicit delete; the owner is the struct's
   destructor, a `move` extraction, or a `unique` field. Rejected: delete-on-non-null (double frees a borrow such
   as `h.raw = &x`), a runtime owned flag (changes struct layout, breaks C / C++ interop records). Not decided:
   whether the chain `outer = h.raw = new PS()` should keep freeing through `outer` (1 / 1 today) or match the
   split form (1 / 0) under the chain = split ruling of the CHAINTERN run.

   IMPLEMENTED 2026-11-18 (this commit): implements PROPOSED (a) -
   a plain pointer parameter adopts an owned store behind a runtime flag that is false at entry. Rows 8 and 10
   become balanced, row 9 stays 2 / 2, the parameter-middle chain is accepted as its split form. Reviewed by the
   main session: 32 edge shapes balanced (branches, loops, switch, early return, back to a borrow, delete and
   re-store, move into a sink or a unique, method, generic); test.sh Release 1998 passed, 0 failed. Residue: a
   by-value lambda capture of an adopted parameter drops the adoption, so `p = new PS(); l = () => { use(p); };`
   leaks (2 / 1) - chosen over the double free the local twin has on master. Three legs changed meaning:
   test_move "chainleak parameter true" 32 -> 33, and err_unique_borrow_into_unique t250MidParam /
   t250DeclParamMid now expect "use of moved variable 'p'".

   SPIKE 2026-11-18 (fields and globals, counts are lower bounds - brace-init stores not probed): refusing every
   owning store into a raw field or global flips 16 Test + 18 example files and 25 core sites (threadpool,
   thread, json, xml, wstring, fft, parallel, spatial) and would refuse the manual-release and user-destructor
   rows. Refusing only `new` into a field of a struct with no user destructor flips 8 Test files + 9 core sites.
   An inferred owning field finds 49 fields with mixed owning and borrowed stores, 19 owning-only fields whose
   struct destructor likely deletes them already (double free), and needs whole-program inference that the
   precompiled core bitcode cannot give. None was built. Related, measured on master: a function returning a
   fresh object into a raw local (`PS* q = make();`) is also never released (1 / 0).
