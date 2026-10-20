# Bare `expect_error("...");` inside a generic instantiation body breaks module verification

Filed 2026-10-19 from the c16 report (pre-existing on master, pointer case too). A bare-semicolon expect_error inside a
generic function body that is instantiated produces "Terminator found in the middle of a basic block" from the LLVM
verifier. The scoped `expect_error("...") { ... }` form works. Sibling of p3 expect-error-in-lambda-body-aborts.
Fix direction: the bare form's scope-close check must not emit its verdict code after the instantiated body's
terminator (emit before the return / in the body's final block). Acceptance: a bare leg inside a generic body passes.
