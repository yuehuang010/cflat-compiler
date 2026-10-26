# CFlat alias of an all-defaults C++ class template: non-constructor type uses still fail

Found by the T469 review (2026-10-24), same on master. T469 resolves `using G = cppt.dflt.Gate;` for the constructor spellings
(`G(1)`, `G<>(1)`), brace init, declarations and `new G(1)`. Type uses that go through other resolvers still fail with
"cannot find the type 'G'": casts `(G*)p`, `sizeof(G)`, and generic arguments `Holder<G>`.
Repro: scratch/repro_keep/box1022/t469/rev_t469_opus_typeuse.cb (header rev_t469_opus_h.h next to it).

Root cause: ResolveCanonicalTypeSpecifier (MainListener_Expressions.cpp ~21447) and ResolveTypeArgEntry do not call
TryResolveDefaultedCxxGenericBaseAlias.

Also same on master (T469 review): an alias of a template with only SOME defaulted params (`using P = rv.Pair;`) is refused with
the vague "the function 'P' is not known"; an alias of a C++ alias template, an alias declared inside a CFlat namespace, and a static
member call through the alias (`G.s()`) are unsupported.
