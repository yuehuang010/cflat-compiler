# C++20 std::barrier type is not available in imported lookup

Summary: `import cpp "barrier"` succeeds, but CFlat cannot resolve the standard `std::barrier` class template.

Repro:
```cflat
import cpp "barrier";
extern int main()
{
    std.barrier sync = std.barrier(1);
    sync.arrive_and_wait();
    return 0;
}
```

Observed: compile error: `cannot find the type 'std.barrier'`.
Expected: construct a one-participant barrier and call `arrive_and_wait()`.
Suspected area: C++ header type registration for `<barrier>`.

Also seen in `test_libs/std/std_20_96_barrier.cb`.
