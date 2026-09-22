# Windows --run static destructor teardown remains unmeasured

The macOS --run smoke failures are resolved. Windows was not measured in that fix round.

The 2026-09-20 fix (49014d6f) interposes `__cxa_atexit` / `__dso_handle` with ORC
LocalCXXRuntimeOverrides, which is Itanium-ABI only. MSVC-ABI code from clang-cl registers
static destructors through the CRT `atexit` / `_onexit` path, which still resolves to host
process symbols. The same run-after-unmap teardown crash may persist under `--run` on Windows.

Probe a header-defined `inline` namespace-scope object with a user destructor, run it with
`--run`, and check the exit code and that the destructor output appears.
