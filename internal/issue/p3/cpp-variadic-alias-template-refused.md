# Non-type expression arguments in C++ alias templates are refused

Residue of the variadic C++ alias template work. A non-type EXPRESSION argument in a C++ alias
template is refused by CFlat, for a pack and for a non-pack parameter alike. Clang folds every
one of them to a value. CFlat has no C++ spelling for the expression, so it reports
"has no C++ spelling" at the use site.

Repros assume `template <unsigned long... I> struct Seq`, `template <int... I> struct ISeq` and
`template <bool... B> struct BSeq`, with the alias declared as shown:

- Pack, addition: `template <unsigned long... I> using PlusSeq = Seq<(I + 1)...>;` used as `PlusSeq<1, 3>`.
  Clang: `Seq<2, 4>`.
- Pack, division: `template <unsigned long... I> using DivSeq = Seq<((I - 5) / 3)...>;` used as `DivSeq<1>`.
  Clang: `Seq<6148914691236517204>` (the unsigned long pack wraps).
- Pack, shift: `template <int... I> using ShlSeq = ISeq<(I << 1)...>;` used as `ShlSeq<3>`.
  Clang: `ISeq<6>`.
- Pack, comparison: `template <int... I> using GtSeq = ISeq<(I > 1)...>;` used as `GtSeq<2>`.
  Clang: `ISeq<1>`.
- Pack, bool negation: `template <bool... B> using NotSeq = BSeq<(!B)...>;` used as `NotSeq<true, false>`.
  Clang: `BSeq<false, true>`.
- Non-pack, addition: `template <unsigned long N> using NP2 = Seq<(N + 1)>;` used as `NP2<1>`.
  Clang: `Seq<2>`.
