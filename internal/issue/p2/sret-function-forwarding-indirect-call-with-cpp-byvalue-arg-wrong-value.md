# `return f(cpptw.Twin(3));` through a Lambda / function<> in a C++-class-returning function gives a wrong value

Found by T319 review 1 (2026-10-16), PRE-EXISTING on master (silent wrong value). Inside a function returning a C++ class by
value, returning the result of an INDIRECT call (Lambda<Twin(Twin)> or thin function<Twin(Twin)>, parameter or local,
capturing or not) that takes a by-value C++ class argument exits 255 instead of 5.

## Repro

```cflat
import cpp "cpp_interop_twin.h";
cpptw.Twin runr() { Lambda<cpptw.Twin(cpptw.Twin)> f = (cpptw.Twin t) => cpptw.Twin(t.value() + 2); return f(cpptw.Twin(3)); }
extern int main() { return runr().value(); }   // 255, clang 5
```

Kept at scratch/repro_keep/box1017/t319/rev_t319_pre_sret_fwd.cb (compile -i Test/library). Binding the call to a local
first, or calling from a function that does not return the class by value, gives 5. T319 made one more shape reach this
(`runn(Lambda<Twin(Twin)> f) { return f(Twin(3)); }` failed verification before T319, now returns 255).

## Fix direction

Return-slot forwarding (caller's sret passed straight to the callee) for an indirect call with a by-value C++ argument:
check that the forwarded slot and the materialized argument temp are not the same storage / ordered wrongly.
