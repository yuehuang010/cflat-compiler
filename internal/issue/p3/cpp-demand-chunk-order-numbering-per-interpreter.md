# C++ demand chunks are numbered per interpreter, so a cache can store a dependent chunk before its dependency

A new demand chunk's order is `demandChunkSources.size() + 1` (chunks in THIS interpreter + 1). A compile that
replays only part of a group's cached chunks numbers its new chunks below a dependency's stored order, and the
replay (stored lists merged, stable-sorted by order) then parses the dependent chunk first - e.g. a wrapper for
`std::cout << "x"` before the chunk that pulled in <iostream>. Mitigated 2026-10-02 (T3b, ReplayCxxDemandChunks
retries a failed chunk after the remaining ones, logged "out-of-order chunk(s) replayed late"), but an inverted
cache is never rewritten, so every warm compile of it pays the late replays (sub-ms each).

Also from the same review: a failed replay attempt keeps the include prelude, "poisoned" entries for failed
template instantiations, and emptied bodies on the explicit-instantiation path (same as a failed live request);
a header template that failed only because of the order could stay refused after its retry succeeds - not seen.

## Fix direction

Number a new chunk one above the highest order already replayed into the interpreter (or the live count, if
higher), so new caches always store dependencies first; keep the retry as the safety net. Consider clearing the
poisoned-instantiation entries a successful retry supersedes.

Found by: T3b review 2, fix timebox 2026-10-02.
