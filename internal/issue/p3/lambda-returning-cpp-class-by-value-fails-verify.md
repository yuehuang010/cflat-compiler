# A Lambda returning a C++ class by value fails module verification

Found by T306 review 3 (2026-10-16), PRE-EXISTING (master and fix alike). `cpptw.Twin` = the counted twin class in
Test/library/cpp_interop_twin.h.

## Repro

```cflat
import cpp "cpp_interop_twin.h";
cpptw.Twin rettw() { return default; }
cpptw.Twin runl(Lambda<cpptw.Twin()> f) { return f(); }
extern int main()
{
    return runl(() => { return cpptw.Twin(cpptw.take(rettw())); }).value();   // module verification failed
}
```

clang++ equivalent (std::function / lambda returning the class by value) compiles and runs.

## Fix direction

Not traced. Check how a Lambda<R()> with a non-trivial C++ class R lowers its return (sret vs direct) against the
call site in runl; compare with a CFlat struct with a destructor as R (does that work?).
