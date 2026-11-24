# CFlat return into same-type initializer: lambda-body callee keeps a copy

Summary: RETELIDE (2026-11-19) builds a CFlat call result straight in a same-type initializer
when a same-type by-value argument is a temporary (`CV z = f(CV(7));`, clang counts), and routes
the implicit move of a returned parameter / `move h->m` through clang's pick when the class has a
forwarding constructor template. NRVO for a single named local already matched clang on master.
What remains is one deliberately kept gate.

Open: a CFlat callee in which a lambda names (captures) a by-value parameter of the returned
class type keeps the extra result copy (LLVMBackend_Overloads.cpp, `LambdaNamesReturnTypeParam`
gate; predicate MainListener_Support.cpp LambdaNamesReturnTypeParam - any Identifier token in a
lambda subtree spelling such a parameter, shadowing treated as a capture). A capture-less lambda,
one capturing something else, nested or dead lambdas are elided like clang. Repro
(Test/test_cpp_interop_bridge.cb `bridge_capture_return`, legs "captured C++ return copy/dtor" at
about line 6219):

    cppi.Tracked bridge_capture_return(cppi.Tracked source)
    {
        Lambda<cppi.Tracked()> captured = () => { return source; };
        return captured();
    }
    cppi.Tracked value = bridge_capture_return(cppi.Tracked(43));

clang++ -std=c++20: copy 2 (capture copy + return copy), dtor 3. CFlat captures `source` by
reference (non-escaping lambda, borrow capture), so the callee makes 1 copy; the kept caller-side
copy brings the total to clang's 2. Same shape with cpcpick.CV: clang v10 b3 k1 c1, CFlat v9 b3 k2
c0 (gate kept), v8 b2 k1 c0 (gate removed).

Needs a maintainer ruling. Principled end state: drop the gate and re-pin the bridge leg to CFlat's
own counts (copy 1, dtor 2), per the lambda value-capture ruling 2026-09-30 (no implicit copy of
non-trivial types); the alternative is a capture that copies like clang.

Not this issue (CFlat sink semantics, recorded for reference): an lvalue passed to a CFlat
`move` parameter (`T f(T a) { return move a; }`, `f(s)`) is consumed by the call, so a class with
a forwarding template constructor builds the parameter through the template (v=13 t=2); clang
copies the lvalue (v=11 k=1 t=1).
