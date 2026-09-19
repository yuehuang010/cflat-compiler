# Bucket p3. A `T**` element makes every C++ member taking it by const reference unsupported

Found 2026-09-19 while fixing
internal/issue/p2/cpp-nested-user-class-pointer-at-inner-depth-unresolvable.md (fix/cpp-nested-user-ptr,
macOS arm64, Release). Pre-existing on master 7f6db1ed, and NOT caused by that fix - measured
identical on both binaries in the same spelling.

## Summary

`std.vector<nest.Cell**>` binds as a type, but its `push_back` is refused, so the container cannot
be filled. The refusal is about the PARAMETER type `nest::Cell **const &`, not about nesting: it
reproduces at depth 1 with no nested specialization anywhere.

## Repro

scratch/nup_o_ppelem.cb in the fix worktree:

```
import cpp "vector";
import cpp "Test/library/cpp_interop_nest.h";
extern int main()
{
    nest.Cell one = nest.Cell(2, 3);
    nest.Cell* p = &one;
    std.vector<nest.Cell**> v = default;
    v.push_back(&p);              // refused
    return (*v[0])->b;
}
```

`nup_o_ppelem.cb(8,4): member 'push_back' of C++ class 'std.vector$.p$.p$nest.Cell' takes
unsupported type 'nest::Cell **const &'`

Identical on master 7f6db1ed and on the fix branch.

## Fix direction

The parameter-type mapper refuses a pointer-to-pointer pointee behind a const reference. Start
from the "takes unsupported type" record and find which arm rejects `T **const &` while `T *const
&` is accepted (the depth-1 pointer element works - fixture leg 1944). Needs its own accept set:
which pointer depths, and which of by-value / const-ref / rvalue-ref parameter shapes, are
supported for a pointer element.

Coverage note: Test/test_cpp_interop_template.cb leg 2404 pins the `std.vector<std.vector<nest.Cell**>>`
TYPE binding only, and says why it cannot push.
