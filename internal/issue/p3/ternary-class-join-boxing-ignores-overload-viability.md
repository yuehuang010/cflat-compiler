# Literal/literal `?:` into a C++ std::string parameter or converting ctor refused

Items 1-3 (receiver calls, two joins in one call, function-value callee) were fixed by TERNJOIN
(2026-11-04; item 4, void* vs string literal, was already refused): a mixed-class `?:` argument boxes to an interface when some candidate taking it is viable for the whole call.

Still refused (clang C++20 accepts and returns 97): a literal/literal ternary into a std::string parameter or a converting
constructor. Repros scratch/repro_keep/t147/stdstring.cb, ctor.cb. Different machinery from the class join: the failure is
in C++ string conversion / converting-constructor lookup for a `?:` of two string literals.
