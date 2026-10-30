# Interface-call overload residue after IFACEOVL

IFACEOVL (83fea586e) landed: interface method calls rank overloads exactly as a direct class call (literal kinds, widths,
pointers, named/defaulted args, returned interfaces, `?.`, generic interfaces). Remaining (repros
scratch/repro_keep/ifaceovl/; direct = the same call on a class receiver):

- Unrelated object pointer accepted: an `Rt*` argument to an interface with `{f(Pt*), f(Qt*)}` calls `f(Pt*)`; the
  direct class refuses (master too). The no-winner fall-through in ResolveInterfaceMethodSlot (LLVMBackend_WinRT.cpp)
  returns the first candidate; it must require an exact or derived pointee.
- Const receiver with overloads `I{f(int); f(double) const}`, `x.f(1)` on a const receiver: interface refuses the
  non-const f(int), direct `const C*` calls it (1), clang picks f(double) const (2). Needs one rule for both paths.
- Pre-existing on both paths (clang: ambiguous): `nullptr` to `{f(Pt*), f(Qt*)}` and `int -3` to `{f(u32), f(double)}`
  pick the first; a `Col : u8` enum argument to `{f(u8), f(Col)}` picks f(u8); `-(1)` picks u8, `(1.0)` picks float.
