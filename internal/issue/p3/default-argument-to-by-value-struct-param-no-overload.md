# `take(default)` to a by-value struct parameter finds no overload

Summary: `int take(S s) { ... } take(default);` reports "no overload of 'take' matches the given
arguments." `default` is accepted at declarations, returns and (since T90) assignment, but overload
resolution has no expected type for it. Found by T90 review r2 (pre-existing on master 2026-10-10).

Repro: scratch/repro_keep/t90/take_default.cb.

Fix direction: when an argument is exactly `default`, let it match any by-value parameter of a
default-constructible type; ambiguity across overloads -> error naming the candidates. Needs a ruling
only if `default` should also bind alias/pointer params (proposal: no).
