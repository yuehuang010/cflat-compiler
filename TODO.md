claude --dangerously-skip-permissions

  ---
Windows ABI gaps:
- WIN32\_FIND\_DATAA is a raw i8\[600] blob with magic offset 44 for cFileName — no header includes
- FindFirstFileA handle typed as i64 to enable -1 comparison against INVALID\_HANDLE\_VALUE
- \_SystemInfo is a hand-mirrored struct; new \_SystemInfo\[1] used as a stack-emulation workaround


Generics limitations:
- hashset/dictionary only support integer keys — no generic hash trait
- sort is O(n²) insertion sort

Thread infrastructure:
vmalloc: For allocating large, virtually contiguous areas of memory.

What's solid:                                                                                       - bond keyword on parameters (including this via method declaration) marks borrow sources

The bug comes from CFlat's automatic ownership semantics conflicting with the program construct's allocator
    lifecycle.

    What happens step by step:

    1. In extern int main(), you write:
    SlabAllocator* sa = new SlabAllocator;
    RunThroughput("SlabAllocator", sa, N);
    2. Inside RunThroughput, b._allocator = sa transfers the raw pointer into the program struct's fat-pointer field.
    When b goes out of scope at the end of RunThroughput, the program's auto-generated destructor fires:
      - Calls sa->cleanup() (releases VirtualAlloc'd memory)
      - Calls C free(sa) (frees the struct itself)

    So sa is now freed. Free #1.
    3. Back in main(), the compiler sees that sa is a local owning pointer created with new. At the end of main()'s
    scope, it generates cleanup code for every owning pointer. For SlabAllocator* (which has a non-trivial destructor),
    it emits:
    call void @"_~SlabAllocator_void_SlabAllocatorPtr_"(ptr %sa)
    3. This calls the destructor on already-freed memory. Free #2 → crash.

    Why only some allocators crash:

    The LLVM IR reveals that MallocAllocator has no generated destructor call at scope exit — CFlat only generates
    cleanup for types that have a non-trivial destructor. MallocAllocator has no custom destructor, so no double-free.
    SlabAllocator, BlockAllocator, and FitAllocator all have custom destructors (they call VirtualFree), so they get the
     double cleanup.

    The fix:

    The IR's cleanup code checks if (ptr != null) before calling the destructor. So nulling the pointer after each
    RunXxx call prevents the second free:

    SlabAllocator* sa = new SlabAllocator;
    RunThroughput("SlabAllocator", sa, N);
    sa = nullptr;  // ownership was transferred; don't destroy again at scope exit

    The root cause is a missing ownership-transfer signal: CFlat doesn't know that assigning sa into b._allocator
    transferred ownership, so it still treats sa as the owner at scope exit.



  2. Buffer placement: heap vs struct-adjacent

  C++: bufA[65537] and bufB[65537] are arrays inside the Stream struct — when Stream is a global, they're contiguous in the BSS segment, and both buffers + all control fields are within a small number of
   cache lines.

  CFlat: _bufA and _bufB are malloc'd separately (stream.cb:43–44) — two heap pointers pointing to arbitrary locations. The struct holds pointer-sized fields; the actual buffer data is elsewhere,
  potentially on different cache lines or pages.
