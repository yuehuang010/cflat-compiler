# `std::chrono` clocks and `std::this_thread` are not addressable from CFlat

## Summary

`import cpp "chrono"` does not make `std.chrono.steady_clock` callable, and
`std.this_thread.sleep_for` is not addressable even when a supported duration value is
available. A C++ bridge is required for ordinary elapsed-time and sleep code. Found
2026-09-16 during the standard-library dogfood session.

## Repro

```cflat
import cpp { "chrono", "thread" } cache;

extern int main()
{
    auto started = std.chrono.steady_clock.now();
    std.chrono.duration<long long> delay = default;
    std.this_thread.sleep_for(delay);
    return 0;
}
```

The first line reports:

```
timing.cb(10,19): 'std::chrono' does not name a C++ class type in the imported headers
```

With the clock line removed, the duration specialization is accepted but the sleep
call reports:

```
probe-thread-sleep.cb(6,4): 'std::this_thread' does not name a C++ class type in the imported headers
```

`std.chrono.duration<long long> delay = default` by itself is already exercised by the
in-repo fixture and works.

## Root cause (hypothesis)

The system-header catalog registers a duration record but does not publish the public
clock classes or the `this_thread` namespace functions. The exact libc++ declaration
filter decision was not traced.

## Fix direction

Expose `steady_clock` and its static `now()` result type, the standard duration aliases,
and the public `this_thread` sleep functions through the C++ catalog. Add an in-repo
fixture row that measures a positive duration and sleeps briefly without a C++ bridge.
