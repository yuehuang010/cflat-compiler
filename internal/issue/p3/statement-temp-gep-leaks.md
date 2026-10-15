# Statement temp behind a GEP argument is conservatively retained in two cases

Summary: two conservative retention cases remain for pointers into statement temps. Neither frees
the temp too early; both can leave a leak.

- (accepted trade-off from T241) an indirect callee that only reads the GEP pointer keeps the temp
  (leak; master freed it), because the indirect target cannot be proven read-only.

- (T245 review 1, accepted trade-off) a later-defined or C++-declared pass-through feeding a reader in the same
  statement, `readL(passL(&(new Temp(27))->v))` / `cxRead(cxPass(...))`, now keeps (leaks) the temp; master freed
  it once. A callee returning its argument counts as keeping it - the earlier-defined shape leaked already.
  Repros scratch/repro_keep/t245_rev/cases/p12.cb, p21.cb.
