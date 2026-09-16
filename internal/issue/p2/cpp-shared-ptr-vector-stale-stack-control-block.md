# A `vector<shared_ptr<ModuleBase>>` element holds a STACK address; destroying it writes into main's frame

## Summary

`Test/test_cpp_interop.cb` (section M78, `register_child` into `cppt::ModuleBase::children`)
leaves at least one `std::shared_ptr<ModuleBase>` element whose control-block pointer is a
STACK address. Destroying the vector performs an atomic refcount decrement THROUGH that
address, i.e. a stray write into `main`'s frame. The suite still exits 0 because the slot it
scribbles is currently dead, so the defect is latent, not benign.

Found 2026-09-16 while adding section M84. PRE-EXISTING: reproduced with the pre-fix compiler
on the unmodified master test file, so it is not caused by the M84 work.

## Repro

```bash
./cmake_build.sh release
x64/Release/cflat Test/test_cpp_interop.cb -i Test/library -o scratch/tci.out
lldb --batch -o "env DYLD_INSERT_LIBRARIES=/usr/lib/libgmalloc.dylib" -o run scratch/tci.out
```

```
stop reason = EXC_BAD_ACCESS (code=1, address=0x1708f7fa8)
tci.out`std::__1::__libcpp_atomic_refcount_decrement<long>:
->  ldaddal x8, x8, [x0]
tci.out`std::__1::vector<std::__1::shared_ptr<cppt::ModuleBase>,
        std::__1::allocator<std::__1::shared_ptr<cppt::ModuleBase>>>::__destroy_vector:
```

`0x1708f7fa8` is main-thread STACK, not heap.

Second observable, without guard malloc: declare a `std.map<std.string, int>` at the END of
main and INSERT through it (a named key, or any key that allocates a new tree node):

```cflat
    std.map<std.string, int> m84counts = default;
    std.string m84key = std.string("compiler");
    m84counts[m84key] = 1;
```

The program dies inside `malloc` with `_os_unfair_lock_unowned_abort` (SIGBUS/SIGKILL,
signal varies run to run). The emitted IR for those three lines is identical to the IR of
the same three lines in a standalone file that runs clean, so the crash is state, not
codegen. A LITERAL-key insert at the same spot survives - it lands on a different frame slot.

## Root cause (hypothesis)

`ModuleBase::register_child(const char*, std::shared_ptr<T>)` takes the `shared_ptr` BY
VALUE. The CFlat call appears to hand it the address of a stack `shared_ptr` slot without a
copy-construct, so the `children` vector retains a pointer into a frame that is later reused.
Not traced further - the minimal standalone reduction (scratch/m78_repro.cb, one
`register_child` of a `make_shared` leaf) does NOT reproduce, so the shape needs
`HolderOf<T>` and/or the deeper M78 nesting.

## Fix direction

Trace the by-value `shared_ptr` parameter of a C++ member TEMPLATE called from a CFlat
`[cpp]` struct constructor and confirm a real copy-construct into the callee's argument slot.
Then assert it: the suite should run clean under `DYLD_INSERT_LIBRARIES=/usr/lib/libgmalloc.dylib`.

## Consequence today

Section M84 is placed EARLY in `main` and inside its own scope for this reason - its locals
are constructed and destroyed before the M78 section runs. Move it back to the end of the
section list once this issue is fixed.
