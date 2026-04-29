claude --dangerously-skip-permissions

  ---
  1. Capturing lambdas (closures)                                                                   
  The current lambda implementation is Phase 1 — only non-capturing. (x) => { x + captured_var }      doesn't work. This is the biggest gap between function<T> being useful and being truly expressive.
   Implementing it requires a capture struct per closure site and a fat-pointer representation (data
   + function pointer), similar to how interfaces work.

  2. ?. on struct member fields

  The memory notes it only works for function arg pointers. CLAUDE.md architecture says it works
  everywhere now — there's a discrepancy worth verifying. If struct member field access like
  obj->child?.field is still broken, it's a quick win since the postfix expression walk already has
  nullConditionalPending.

  3. foreach + IEnumerable<T> protocol

  IEnumerable<T> is defined in interfaces.cb but foreach uses duck-typed count()/get(). Tying them
  together (so any IEnumerable<T> works in foreach) would make the interface system feel complete
  and enable lazy iterators.

  6. bond keyword / borrow annotations

  The TODO sketches this out — marking parameters as borrows with lifetime tracking. This is the
  next big language design step after move.

Windows ABI gaps:
- WIN32\_FIND\_DATAA is a raw i8\[600] blob with magic offset 44 for cFileName — no header includes
- FindFirstFileA handle typed as i64 to enable -1 comparison against INVALID\_HANDLE\_VALUE
- \_SystemInfo is a hand-mirrored struct; new \_SystemInfo\[1] used as a stack-emulation workaround

Generics limitations:
- hashset/dictionary only support integer keys — no generic hash trait
- list.removeAt now auto-frees pointer elements via is_pointer(T) + if const
- sort is O(n²) insertion sort

Thread infrastructure:
- __ThreadArgs trampoline struct because CFlat closures can't be passed as Win32 callbacks
- malloc used directly (bypasses tracked allocator) for thread args
vmalloc: For allocating large, virtually contiguous areas of memory.

Now I understand the root cause. ParseIdentifier doesn't set TypeAndValue for global variables, so calling .alloc() on a
BlockAllocator\* global fails. The fix is to add helper functions in runtime.cb that accept BlockAllocator\* as a parameter —
function arguments DO carry TypeAndValue, so method dispatch works.

Add a check to ensure circular import doesn't hang.
Investigate why Claude keep using null instead of nullptr.

One known limitation: embedding mutex inside a struct leaves the field uninitialized. Workaround: use a separate local
 mutex and pass its address as void\*.

4. Vale: Region-Based Memory
Groups objects into regions with a single owner. Borrows are region-scoped — you can borrow freely
 within a region but can't smuggle a reference out. Simpler than Rust lifetimes but less
expressive.

5. Cone: Permission Types

Each reference has a permission: uni (unique owner), iso (isolated), imm (immutable shared), mut
(mutable). The type system tracks which permissions are compatible, preventing aliased mutation or
 dangling borrows.

What's solid:                                                                                       - bond keyword on parameters (including this via method declaration) marks borrow sources

- Return type is implicitly bonded — no annotation on the return                                    - span<T> with compile-time sizeof(T) — encoding/size is a type property
- span<T> internal representation (offset type, size cap) is a library decision, not a language
constraint
- bond and span<T> are independent concerns — safety tracking doesn't depend on representation

The open question:
- Can a bonded value be stored in a struct field or heap allocation? Answering yes pulls you
toward Rust-style lifetime parameters on structs. Answering no is simpler but limits what you can
express.

### Known Limitations
- **Null-conditional (`?.`)**: Only for pointers passed as function arguments — not local variables or struct members
- **Generic struct defaults**: `V x = default` in function locals and nested generic fields may not codegen correctly; field-level `= default` works
- **`move` on `removeAt`**: Owned pointer elements are not auto-freed on removal from a collection

That's the one hard problem left to solve.
