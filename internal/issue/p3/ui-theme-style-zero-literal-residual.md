# UI Style and Theme fields still conflate numeric zero with unset

The native window API and timer issues are fixed, but the settled UI color contract
also requires raw numeric color `0` to remain a valid literal. `ListCellStyle` already
has an explicit `hasLiteral` tag and satisfies that contract. Public `Style` and
`Theme` integer fields still use `0` as the legacy unset/default sentinel in
`pickColor` and host theme resolution.

The remaining design work is to introduce explicit presence/semantic-role storage
for Style and Theme without breaking existing field-level app code. CFlat direct
assignment currently cannot distinguish a zero-filled default struct from an app
assignment such as `theme.panelBg = 0`; a tagged setter/table or an equivalent
language-supported representation is required before this item can be closed.
