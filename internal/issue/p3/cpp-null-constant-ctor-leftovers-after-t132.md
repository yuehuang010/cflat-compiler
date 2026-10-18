# Literal-zero C++ constructor leftovers after T132

PRE-EXISTING / follow-ups (T132 reviews 1-3, 2026-10-12). Items 1, 2, 4 and 6 of the original list
are closed by T140 (NULLLEFT); items 1, 2 and 4 are fixed, item 6 did not reproduce on the T140 build.

3. `r.SCo(300)`, `takeSCo(0 | 300)` (char-only ctor) refused by CFlat's no-implicit-narrowing rule;
   clang accepts (12 / 11 / 12). By design unless ruled otherwise.
5. A CFlat struct field default of a C++ class type with `= 0` is not applied (`r.K k = 0;`,
   `r.SInt s = 0;` fields read 0; clang 1 / 11); field `r.S7 s = 0;` accepted where clang is ambiguous.
7. A deleted or private narrowing sibling is not counted in the tie: `D(long)` plus deleted `D(char)`
   and `PR(long)` plus private `PR(char)` pick the long ctor for `new X(5)` and by-value `takeX(5)`
   (1); clang reports ambiguity. A deleted char ctor beside a long ctor returns 71 on the same path.
8. `new K{0}` (K with a null-taking ctor) is refused by CFlat; clang returns 1.
9. By-value equal-rank widening tie `rv.takeW(h)` (W(long)/W(double)/W(char), short h) reports
   "no overload of 'rv.takeW'"; clang reports "conversion from 'short' to 'W' is ambiguous". The
   refusal is right; the text differs.
10. Same-named free functions in two namespaces (`t140::takeK` and `rv::takeK(t132::K)` in one TU):
   `t140.takeK(0)` fails to bind ("clang: no viable conversion from 'int'"); alone it returns 1.

## Widened 2026-10-16 (T300 review, pre-existing)
Item 5 is not specific to `= 0`: ANY scalar initializer on a C++ class field default (`struct W { IntC c = 5; }`, `IntC c = 2.5`)
reads 0 - no ctor runs. Once fixed, a floating initializer through an integer ctor must refuse (ruling 2026-10-13).
Probe: scratch/repro_keep/wt_t300/rev_t300_opus_probe.sh.
