# A `T*` argument binds a `T**` parameter and the program SEGFAULTS

The reverse direction of `double-pointer-arg-binds-single-pointer-param.md`, split out when that
issue's depth gate landed. Measured on BOTH the merge-base binary (a4a90a5) and the post-fix
binary: compiles clean, links, and dies with rc 139 (SIGSEGV) at the callee's dereference.

```cflat
class Circle { int r = 0; };
int byPP(Circle** c) { return 2000 + (*c)->r; }

extern int main()
{
    Circle* a = new Circle(); a->r = 3;
    return byPP(a);             // a Circle*, not a Circle**
}
```

`(*c)` loads the first 8 bytes of the `Circle` object as if they were a pointer and dereferences
them. Unchanged by the depth gate, deliberately.

## Why the landed gate does not cover it

The gate is one-sided: it refuses a PROVEN `T**` argument at a PROVEN `T*` parameter. The mirror
rule would have to read `ElemPointer == false` on the ARGUMENT as a claim that the argument is
depth 1, and it is not one - `ElemPointer` is populated only by source parsing and C interop, so
`false` means NOT RECORDED (the standing `""`/`0`-is-not-a-negative-claim rule).

That is not a theoretical objection. This program is CORRECT and must keep compiling, and its
argument carries exactly the same `arg{Circle p=1 ep=0}` as the broken one above:

```cflat
int firstR(Circle** cs) { return cs[0]->r; }
extern int main() { Circle*[2] arr = default; arr[0] = new Circle(); arr[0]->r = 7;
                    return firstR(arr); }        // rc 7 on both binaries; frozen as a value leg
                                                 // "pd_ptr_array_slot_into_ptrptr_param"
```

So the mirror gate needs a POSITIVE proof of depth 1 on the argument, which the type model does not
currently carry. Fix direction: a real `PointerDepth` int on `TypeAndValue` (`0` = not recorded,
matching `FuncPtrParam::PointerDepth`), populated at both `ParseDeclarationSpecifiers` copies, and
added to the `--init` cache round-trip in `LLVMBackend.cpp` in the same change. Only then can the
reverse direction be refused without false-rejecting the array-slot spelling.
