# Generic instantiation used as a value: leftovers after T191

Pre-existing on master (T191 Opus r1, 2026-10-14). Probes: cflat-fix-t191 scratch/rev_t191_opus_probe.sh (copied to scratch/repro_keep/t191_rv1/).
- Overloaded generic `ov<int>` (1- and 2-param `ov<T>`): name lookup ignores the slot's parameter count and binds the 2-param
  version; `function<int(int)> g = ov<int>;` is refused ("different signatures"), clang-style overload pick by target type expected.
- Wrong type-argument count `k ? wrap<int, int> : t1` compiles without an error and crashes at run time (exit 133).
- `bool b = wrap<int>;` gives false; a named function gives true.
Related: p3/generic-function-name-as-pointer-argument (return wrap<int> verifier failure).
