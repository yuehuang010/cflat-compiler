# q05 - Enums as a first-class type (full mode, single issue)

One member, four legs behind one declaration-pass change. Full mode: it registers a new type
kind in both `ParseDeclarationSpecifiers` copies and the scanner, which is name-resolution
territory. Retire the bucket when it lands.

| # | Item | Status | Shape |
|---|------|--------|-------|
| 1 | `p3/enum-not-a-first-class-type` | READY | pre-register the enum in ForwardRefScanner as a scoped type alias to its base + member constants; member lookup via `FindFirstVisibleScoped`; APInt initializer fold with a range error; base signedness drives sext/zext. Legs in Test/test_c.cb, Test/test_basic.cb, Test/errors/err_switch_case_range.cb. |

Land before q06 member 3's enum leaf. Constraints: both `ParseDeclarationSpecifiers` copies;
`--init` serializer rule if a TypeAndValue field is added; `LogErrorContext` + `LocalizeMessage`;
no `ctx->getText()` provenance.
