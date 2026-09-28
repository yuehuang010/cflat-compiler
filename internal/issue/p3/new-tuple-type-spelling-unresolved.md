# `new (int, double)()` - tuple type spelling not resolved in `new`

Pre-existing (found 2026-09-27 by the fix/sizeof-values review): `new (int, double)()` -> "cannot find the
type '(int,double)'"; `new tuple<int, double>()` works. `new` resolves its type through
ParseTypeSpecifierName (MainListener_Expressions.cpp ~16655), outside the ParseTypeName tuple branch that
fix/sizeof-values added for sizeof/casts. Fix direction: route the tuple specifier through the same helper.
