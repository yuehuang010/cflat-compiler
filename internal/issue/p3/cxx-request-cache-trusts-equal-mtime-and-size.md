# Cached C++ request outcomes trust restored old mtime and size

PRE-EXISTING (T70 review 4, narrowed after T108 round 2). Dependency validation detects ordinary
mtime drift and same-stamp edits near the recorded dependency-hash time. It cannot detect an edit
that restores the original old mtime and keeps the same size: the stored dependency hash is still
trusted. Probe A demonstrates stale replay (seed returns 45, edited header should return 46).

Repro: compile once to seed a C++ request cache with an included header stamped in 2020. Change
the header contents without changing its byte count, restore the saved 2020 timestamp, then compile
again against the same cache. The request outcome replays from the old content hash. Git has the
same limitation when a user deliberately restores a file's prior mtime.

Needs maintainer ruling: accept the git-equivalent restored-mtime limitation, or hash NEG and
closure-restore dependencies on the warm path. One FNV-1a pass over the dependencies in the
`test_cpp_interop` cache measured 1,187 unique files / 7.4 MB / 0.590 s on this host. Do not add
whole-content hashing to every warm compile without that ruling.
