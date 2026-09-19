# Bucket p3. A C++ reference-to-pointer parameter is ranked DEPTH-BLIND in overload resolution

Found 2026-09-19 while fixing
internal/issue/p3/cpp-pointer-to-pointer-element-parameter-unsupported.md (fix/cpp-ptrptr-param,
macOS arm64, Release). The answer is measured IDENTICAL on master 33c97f1b and on that branch;
that fix makes the better candidate exist for the first time, it does not change the pick.

## Summary

For a parameter carrying `IsCxxRefToPointer`, `LLVMBackend_Overloads.cpp` accepts ANY pointer
argument (`result = candidateParamItr->IsCxxRefToPointer && arg.TypeAndValue.Pointer ? 0 : ...`,
~line 693) and exempts the parameter from the depth gate (`!candidateParamItr->IsCxxRefToPointer
&& arg.TypeAndValue.PointerDepthRefuses(...)`, ~line 805). The `tmpParam.ElemPointer = false`
normalization at ~line 457 also predates a ref-to-pointer parameter that legitimately keeps
`ElemPointer` (a `T **&`), and is now wrong for that shape - though it is a no-op for the depth-1
shape it was written for.

## Repro

scratch/ppp_o_ovl.h + scratch/ppp_o_ovl.cb in the fix worktree:

```
namespace po {
inline int pick(nest::Cell *&p)  { return 1; }
inline int pick(nest::Cell **&p) { return 2; }
}
```

`po.pick(p)` -> 1 (right). `po.pick(pp)` with `nest.Cell** pp` -> 1, should be 2.

## Fix direction

Stop stripping `ElemPointer` at ~457, and let the depth gate run for a ref-to-pointer parameter.
Needs its own accept set before the gate goes in: enabling a depth REJECTION in the ranker is the
false-rejection family the lessons file warns about, and `T*&` parameters today accept arguments
of any depth. Freeze the currently-binding call shapes as value legs first.
