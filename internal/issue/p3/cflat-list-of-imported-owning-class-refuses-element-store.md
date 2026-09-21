# Bucket p3

Summary: a CFlat generic `list<T>` cannot store an imported owning C++ class by
value, even when the class has copy and move constructors.

Minimal repro: `scratch/tw_list_generic.cb` and its twin-specific probes instantiate
`list<T>`, create `T value = default`, then call `values.add(value)`.

Measured on the Release binary built before this audit:

    CFlat compile=0 run=0: result=4 ctor=1 copy=1 move=0 dtor=2 live=0
    C++   compile=1 run=N/A: cannot assign to C++ class 'cpptw.Twin' from this expression

Fix direction: route list element stores through the imported class copy/move
contract instead of the native CFlat assignment path.
