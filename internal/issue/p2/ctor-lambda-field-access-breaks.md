# A lambda inside a constructor cannot reach the struct's fields

Summary: inside a native constructor body, a lambda that writes a field fails:
- `this.id = i;` in the lambda -> "Module verification failed: Referring to an instruction in another function!"
  (the capture reuses the ctor's `this` value directly instead of capturing it). Release exe; would assert on Debug.
- bare `id = i;` in the lambda -> "Undefined variable id." (implicit-this field lookup is not done in a ctor lambda).
The same lambda inside an ordinary method (`void Set(int i)`) compiles and runs. Pre-existing: reproduces on
pre-const-phase-0 builds (1903f02fd). Found during const phase 2.

Repro: scratch/repro_keep/ctor_lambda/this_field.cb, bare_field.cb (`struct S { int id = 0; S(int i) {
Lambda<int()> f = () => { this.id = i; return 0; }; f(); } }; int main() { S s = S(4); return s.id; }`, expect 4).

Fix direction: route ctor-body lambdas through the same `this` capture and implicit-field lookup the method path uses;
the verifier failure needs a real capture, not a guard. After the fix, re-check the const phase 2 ConstructingScope
exemption: a const field written from a ctor lambda must still be refused (#2, err_const_field_write.cb).
