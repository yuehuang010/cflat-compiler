# Cached C++ request outcomes survive a header edit that keeps mtime and size

PRE-EXISTING (T70 review 4). Dependency validation trusts equal mtime + size
(LLVMBackend_StateAndImports.cpp ~3032) and the request replay restores the old dependency hashes
(LLVMBackend_CInterop.cpp ~25263), so the closure-hash key of a wrapper request does not change
either. Any cached wrapper outcome - positive or NEG - replays stale after such an edit.

Repro (T70 door): warm a refusal for `operator C*() &` passing `std.move(c)` to a `C*` param, then
edit the transitively included header to `operator C*() &&` keeping mtime and size; a warm compile
still refuses ("cannot pass C++ class xvalue"); fresh cache, master and clang++ accept (value 31).
Touching the header fixes it. Normal edits invalidate correctly.

Fix direction: validate dependency content (hash) when mtime+size match but the request is a NEG
or a closure-hash restore, or hash all dependency bytes on a cheap path. Cost-check with
scratch/libs_perf.sh (warm budget).
