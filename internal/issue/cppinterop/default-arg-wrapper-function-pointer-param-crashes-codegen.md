# Default-argument wrapper spells a function-pointer parameter as `T (*)(..) a1`; clang CodeGen aborts

Found 2026-09-07 by the Dear ImGui headless spike. After the record-layout fixes, the first
call into ImGui (`ImGui.CreateContext();`, or any body at all) exits 133 in Release with no
output; Debug says `UNREACHABLE executed at clang/lib/CodeGen/CodeGenTypes.cpp:598 -
Unexpected placeholder builtin type!`.

## Repro

```cflat
import cpp "imgui.h";      // -i scratch/imgui_src, --cpp-assume-noexcept
extern int main() { ImGui.CreateContext(); return 0; }
```

## Root cause

`BuildCxxDefaultWrappers` (LLVMBackend_CInterop.cpp) emits one forwarding wrapper per
omitted-default arity: `extern "C++" { static R name_cpp(<type> a0, <type> a1, ...) { ... } }`.
For `ImGui::PlotLines(const char*, float (*)(void*, int), void*, int, ...)` that spells
`float (*)(void *, int) a1`, which is not a declarator. Clang reports "expected ')'" and
"use of undeclared identifier 'a1'", the extraction consumer swallows all errors by design,
`EmitCxxDefinitions` resets the error tally, and the RecoveryExpr-laden body reaches CodeGen
(phase 1 `HandleTopLevelDecl` of the `extern "C++"` LinkageSpecDecl), which trips the
placeholder-type unreachable. Array and pointer-to-member parameter types have the same
spelling hazard. Backtrace and dumped stub: scratch/imgui_spike/stub.cpp lines 186-215.

## Fix direction

1. Spell wrapper parameters through an identity template (`typename __cflat_pid<T>::type a1`)
   or a declarator-insertion helper.
2. `EmitCxxDefinitions` must never hand CodeGen a decl that `isInvalidDecl()` or whose body /
   initializer `containsErrors()`; walk a LinkageSpecDecl member by member.
3. A wrapper that still fails Sema is dropped and its CFlat declaration is not registered, so an
   omitted-argument call gets the existing "default argument ... unsupported type" LogError
   instead of a link failure.

## Status 2026-09-07 (working tree, uncommitted)

All three parts landed (Codex, brief scratch/CPP_DFLT_WRAPPER_BRIEF.md): identity-template
parameter spelling, `EmitCxxDefinitions` skips invalid / error-carrying decls and walks a
LinkageSpecDecl member by member, a dropped wrapper is reported in
`ExtractResult::droppedCxxDefaultWrappers` and its default is marked "unsupported" so no CFlat
declaration is registered (header cache v34). Fixture: `default_fn_arg` / `default_array_arg` in
Test/library/cpp_interop_basic.h, called from Test/test_c_interop.cb.

Three follow-on findings from the same spike, also fixed in the working tree:
- `= NULL` on a pointer parameter is clang's `__null`, which evaluated as an int and hit the
  pointer-typed parameter as "unsupported type". `DefaultArgumentOf` now classifies any null
  pointer constant on a pointer parameter as `nullptr`, and any other pointer default as nonconst.
- `alias ImGuiIO io = ImGui.GetIO();` was routed into `TryDeclareForeignCxxLocal`, which demands
  a callable destructor for a slot it never needs. An `IsAlias` declaration now takes the ordinary
  alias-binding path (no slot, no constructor, no destructor).
- `ImGui.Button("press")` resolved to the two-parameter original (declaration order) instead of
  the exact-arity default wrapper, then failed on the non-constant `ImVec2(0, 0)` default.
  `ComputeOverloadFunction` now prefers the candidate with fewer default-filled parameters in
  both the perfect and the promotion/implicit tier.

