# p3: field copy leftovers after R22 (both reproduce on master and on R22)

Filed at R22 landing (2026-10-27).

1. Leak: `string t = this->vals[0];` and `string t = h.vals[0];` (vals = owning `list<string>` field) leak one buffer
   (allocs/frees 2/1, leak 1). The implicit-this spelling `vals[0]` is balanced after R22 (2/2). Different root cause
   from R22 (explicit member access + overloaded alias operator[]). Repros scratch/repro_keep/r22/idx_this.cb,
   idx_external.cb.
2. False refusal: `t = h->name;` where `h` is a borrowed pointer parameter and `t` an existing local is refused with
   "cannot consume field ... through borrowed parameter"; a borrowing assignment copies in C++ and should be accepted
   (copy), like the declaration form.
