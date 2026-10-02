# A `nullptr` value is typed void* at C++ call boundaries: picks void* overloads, nullptr_t params fail

Found by the ST4 review (2026-10-01), pre-existing on master. ST4 fixed std::nullptr_t as a TYPE
(template arguments, declarations); a nullptr VALUE (literal or a variable of that type) is still
spelled `void*`, so `f(nullptr)` with overloads f(void*) / f(std::nullptr_t) picks void* (clang:
nullptr_t), and a nullptr_t-only parameter refuses the call. Probes (copied from the reviewer):
scratch/repro_keep/st4_preexisting/null_pick.cb, null.cb. Fix needs a nullptr_t value type at the
C++ call spelling (decltype(nullptr)), not void*.
