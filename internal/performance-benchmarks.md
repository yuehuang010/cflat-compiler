---
name: performance-benchmarks
description: CFlat performance benchmark setup, files, reference throughput numbers, and stream/channel design notes
metadata:
  type: reference
---

# Performance Benchmarking

## Running benchmarks

Always use `performance.bat` - it compiles all benchmarks with `-O2` and runs both the CFlat and C++ reference variants:

```bash
performance.bat
```

Never compile benchmarks manually without `-O2`. Unoptimized numbers are misleading.

Manual compile (if needed):

```bash
x64/Debug/cflat.exe performance/perf_yes_compare.cb -i cflat/core -o performance/perf_yes_compare.exe -O2
```

Pin producer and consumer to separate cores with `Thread.setAffinity(u64 mask)` or `prog._thread.setAffinity(mask)` to avoid OS scheduling jitter.

See also `internal/project_host_cpu_cache_topology.md` for dev-machine CPU topology notes (Ryzen AI 9 365, heterogeneous Zen5/Zen5c).
