# Variadic C++ alias template leftovers after T329

T329 (2026-10-16) expands a trailing pack that the alias target spells `P...` (ISeq<0,1,2>, ISeq<>, TypeList<T...>,
Pair<H, TypeList<T...>>). Still refused (master identical):
- a pack the target never expands: `template<class... T> using Fixed = Single<int>;` (arity refusal). T329 infers "pack"
  from the pattern spelling only; needs a pack flag from CClangExtract.cpp (HarvestAliasTemplatePattern ~4540,
  VisitTypeAliasTemplateDecl ~4620) - a new field there needs the header-cache serializer.
- a pack that is not the last parameter (`template<class... T, class U>`), not measured.
- (T329 review 1) pattern expansion `template<class... T> using Proj = TypeList<Box<T>...>;` refused with a garbled
  "TypeList<Box<int>::::::>" text (clang 1); `Mix = Pair2<TypeList<T...>, TypeList<Box<T>...>>` now fails with that text
  instead of the arity message. An alias using only `sizeof...(T)` (`SzOnly = Val<sizeof...(T)>`) gets the arity refusal.
