# C++ const pointee dropped at pointer-variable init; writes through a returned `const Out*` SIGBUS

Filed 2026-10-14 from the T168 Opus r3 review (PRE-EXISTING on master and T168).

## 1. Pointer-variable initialization drops const
`int* p = k_cptr();` (k_cptr returns `const int*`), `KRec* rp = &k_cr;`, `In* ip = &k_out.in;` (const C++ globals), and since T168
the array forms `KRec* rp = k_crecs;`, `int* p = k_ka;` all compile and run; clang++ -std=c++20 refuses (invalid conversion
drops const). Argument passing already refuses (CxxPointeeConstRelation Drops at the four overload call sites); the declaration
/ assignment conversion does not consult it. Writes through p then land in read-only storage (SIGBUS) for constexpr objects.

## 2. Writes through a returned const record pointer
`p->in.x = 5`, `p->y = 1`, `k_outp()->in.x = 5` with `const Out* k_outp()` -> SIGBUS 138 (master too); clang refuses at compile
time. `(*k_outp()).in.x = 5` is refused since T168. Fix: carry IsCxxPointeeConst onto `->` field addresses of a call result like
the `(*x).` path (MainListener_PostfixExpression.cpp ~2499/2518).

Ruling basis: cpp-const ruling 2026-09-27 (writes through C++ const results refused). Repros scratch/repro_keep/t168_rv3/.

PARKED 2026-10-14 (T184, branch fix/t184-cxxconstinit, worktree kept, uncommitted r2):
- Item 1 (refuse const-dropping init / assign / return) breaks Test/test_cpp_interop.cb:8019 (`char* p = s.c_str();`) and
  Test/test_cpp_interop_template.cb:3432 (`nest.Cell* p = constref_const_return();`): both are invalid C++ (clang refuses).
  RULING NEEDED: does refusing const-dropping pointer init conflict with the const-unenforced / "CFlat exposes no const" rulings?
- Item 2 (`->` writes through a const pointee, SIGBUS) is fixed in the worktree (four IsCxxPointeeConst terms,
  MainListener_PostfixExpression.cpp) but breaks Test/test_cpp_interop.cb:12340 `mtcAutoP->v = 18;` where
  `auto mtcAutoP = mtcH.cpr();` returns `const MtConstObj*&` - clang refuses that write too. RULING NEEDED: may that leg be
  corrected (it encodes the defect per the 2026-09-27 cpp-const ruling)? Also: r2 added Test/library/cpp_t184_const_pointee.hpp
  (new header - fold into cpp_interop_basic.h before landing).
