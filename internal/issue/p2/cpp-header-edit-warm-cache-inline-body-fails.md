# After a warm build, editing an imported C++ header makes the next compile fail an inline body

Found by ST4 (2026-10-01) and hit again by the main session on fix/std-impl-return-types;
pre-existing on master (reproduced with master's exe on master's tree, script copied to
scratch/repro_keep/st4_preexisting/r3_mt.sh if present).
Steps: compile Test/test_cpp_interop.cb (warm), edit Test/library/cpp_interop_basic.h (or another
header in the same import set, e.g. cpp_interop_opaque_return.hpp), compile again:
`import cpp: the C++ definitions this program uses could not be generated: clang: failed to compile
inline body 'cpp_record_static_call' required by this program` at test_cpp_interop.cb(969).
A fresh CFLAT_CACHE_DIR, or a second compile, passes. Suspect: the demand companion / request
sidecar keyed on the old header stamp is replayed against the edited header's group. Rebuild
equivalence must hold: edited header -> same result as cold.
