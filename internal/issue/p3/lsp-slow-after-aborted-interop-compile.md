# LSP analysis of test_cpp_interop takes ~491 s after an aborted interop compile

Measured 2026-10-22 01:55 (Mac): after an interop compile was killed mid-run (memory pressure), the LSP bulk
sweep's cold analysis of Test/test_cpp_interop.cb took ~491 s (50k C++ request misses, re-harvest per demand)
and hit the sweep's 300 s timeout. Controls: stale-stamp cache 54 s, empty cache 88 s, normal warm 20 s. So an
aborted compile leaves cache state that is worse than an empty cache (likely partial entries that miss on
every demand instead of being rebuilt once).

Fix direction: make request-cache writes atomic per entry (write temp + rename) or validate entry
completeness on load and treat a partial entry as absent; add a probe that kills a compile mid-harvest and
checks the next LSP analysis time is within the empty-cache bound.
