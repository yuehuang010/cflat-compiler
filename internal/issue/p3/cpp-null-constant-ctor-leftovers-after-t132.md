# Literal-zero C++ constructor leftovers after T132

PRE-EXISTING / follow-ups (T132 reviews 1-3, 2026-10-12). Items 1, 2, 4 and 6 of the original list
are closed by T140 (NULLLEFT); items 1, 2 and 4 are fixed, item 6 did not reproduce on the T140 build.
Items 5, 7 and 8 (incl. the 2026-10-16 widening) are closed by T310 (FLDINIT): legs in
Test/test_cpp_interop.cb (T310 block) and Test/errors/err_cpp_hidden_ctor_tie.cb, err_cpp_field_default_scalar_refused.cb.

3. `r.SCo(300)`, `takeSCo(0 | 300)` (char-only ctor) refused by CFlat's no-implicit-narrowing rule;
   clang accepts (12 / 11 / 12). By design unless ruled otherwise.
9. By-value equal-rank widening tie `rv.takeW(h)` (W(long)/W(double)/W(char), short h) reports
   "no overload of 'rv.takeW'"; clang reports "conversion from 'short' to 'W' is ambiguous". The
   refusal is right; the text differs.
10. Same-named free functions in two namespaces (`t140::takeK` and `rv::takeK(t132::K)` in one TU):
   `t140.takeK(0)` fails to bind ("clang: no viable conversion from 'int'"); alone it returns 1.
