# P3: define-group ODR error missing in function parameter / return positions

Found 2026-10-01 by the V3 review (define-group specialization mismatch error, ruling 2026-09-30).
Probes: scratch/repro_keep/v3_rev/ (main checkout): fn_param.cb, fn_ret.cb.

`int f(cppd.Layout<long>* p)` or a function returning `cppd.Layout<long>`, with
`import cpp "cpp_interop_define_primary.h"; import cpp "cpp_interop_define_special.h" define
"CFLAT_DEFINE_SPECIAL";` is refused, but only with "cannot find the type 'cppd.Layout<long>'" - the
ODR text naming both headers and defines never appears. Locals, fields and nested uses get it.

Likely cause: the signature pass requests the type silently (ForwardRefScanner.cpp ~293) and the C++
error text is dropped; the generic unknown-type error (LLVMBackend_Lookup.cpp ~438 /
LLVMBackend_VariablesAndIR.cpp ~2015) is all that is left. Fix where the signature path reports.
Acceptance: param + return legs in Test/errors/err_cpp_template_no_match.cb expecting "violates the C++ ODR".

Also (nit): the `'|'` separator handling added in CxxTemplateDefineMismatch (`entry.find('|')`) and in
RequestCxxTypeInOwningGroup (`name.resize(separator)`) is dead - CClangExtract never emits '|'. Remove.
