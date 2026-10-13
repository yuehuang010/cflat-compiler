# Calling an inline variadic C++ function from an imported header fails to link

Found in T78 round 2b (pre-existing on master). Repros: scratch/repro_keep/t78/rev_t78_r2_vin.cb
(+ .cpp oracle), vin.cb with t78/var/vq.h.

`inline double vin(double, ...)` defined in a header imported with `import cpp`: the call links
with an undefined symbol `vq::vin(double,...)`; the non-inline form links; clang links and runs.
The inline body is never emitted (the demand/body path likely skips variadic functions).
T78 changed `takeVarD` in Test/library/cpp_t78_scalar_table.h to non-inline to avoid it.
