# Objects whose ctor keeps a `this`-capturing lambda cannot be returned or relocated by value

Leftover of T407 (p1 ctor-lambda-field-access-breaks, fixed: ctor lambdas capture `this` like method lambdas).
A native ctor whose lambda captures `this` and keeps the closure (field, local then field, nested lambda,
container, global) marks the struct (StructData::StoresThisClosure, decided on the ctor's emitted IR). Every
memberwise copy, by-value return, fresh temporary moved into existing storage or a by-value parameter, and a
by-value ctor result of a holder is refused at module end (LLVMBackend_OwnershipTemps.cpp T407 block).

Still refused, could be made to work:
- by-value return (`S make() { return S(4); }`, `return a;`): needs sret-style in-place return for native
  closure-bearing results (the r2b-r5 ABI attempt on fix/t345-ctorlambda regressed ownership - do not reuse it).
- a holder of such a type without its own closure field (`struct H { S s = default; }`): its ctor is not sret
  (IsNativeValueConstructor looks at direct closure fields only), so H is refused at its definition.
- `list<S>` of such a type: list's instantiated copy paths copy S, refused at the list<S> use.

Accepted by ruling but still stale: an explicit move (`S b = move a;`, `take(move a)`) keeps the closure
pointing at the moved-from storage (no ASan report while that frame lives). A by-value parameter from an
lvalue is a borrow (no copy), its closure sees the caller's object - same value as the C++ twin.

Conservative: a this-capturing closure passed to any callee other than its own invoker or closure helpers,
or stored outside a local, marks the struct (refuses copies even where the closure does not reach the object).
