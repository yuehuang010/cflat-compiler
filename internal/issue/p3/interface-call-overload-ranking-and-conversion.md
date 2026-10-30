# Overload calls through an interface variable rank and convert differently from direct calls

Filed 2026-10-29 from the NEST6 review 2 (Opus). The behaviour is the same on master before and after NEST6, which
did not touch it. The direct-class calls already match clang. Repros and the probe script are in
scratch/repro_keep/nest6/ (rev_nest6_opus_repro/, rev_nest6_opus_probe.sh, nest6_review2.md).

## Repro shapes (`I p = c;`, c is a class implementing I)

- `I { int f(int); int f(char); int f(float); int f(double); }`: `p.f(1)` picks f(char) and `p.f(1.5)` picks
  f(float). The direct class call and clang pick f(int) and f(double).
- `I { int f(long); int f(short); }`: `p.f(1)` picks f(short). The direct class call picks f(long). clang says the
  call is ambiguous.
- A non-convertible argument gives "module verification failed" (an LLVM verifier abort) instead of a LogError.
  Cases: `f(int)` given 1.5, `f(float)` given a double, `f(int)` given a struct, and named `z: &q` to an S param.
- `g(int x, int* z)` given `"str"` is accepted. The direct call refuses it.

## Fix direction

Route interface-variable overload resolution through the same ranking and conversion checks as the direct class
call (the NEST6 tiedOut path in LLVMBackend_Overloads.cpp is the shared entry). Report a bad conversion with
LogError before the IR is emitted.
