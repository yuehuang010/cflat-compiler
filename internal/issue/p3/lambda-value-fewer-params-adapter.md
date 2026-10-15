# A Lambda<> VALUE with fewer params than a wider Lambda<> slot is refused (stopgap)

T195 (2026-10-14) refuses `Lambda<int(int)> L = ...; Lambda<int(int,int)> M = L;` because a closure's environment is its last
argument and the static Lambda<> type does not say whether L captures (the capturing form crashed on master; the
non-capturing and named-function-in-Lambda forms ran correctly on master - r01/v02 in cflat-fix-t195 review probes,
copied to scratch/repro_keep/t195_rv1/). The fewer-parameter ruling says this bind is legal.
Fix direction (Opus T195 r1): at the bind site, lower the value into an adapter closure that captures the value by value and
takes the slot's parameters (reuse the T189 padded-literal invoker path), calling the inner value with its own arity.
Needs a maintainer OK on the copy semantics (the adapter copies the fat value; captures are shared/owned per the
lambda-value-capture ruling). Workaround today: `Lambda<int(int,int)> M = (int a, int b) => { return L(a); };`.

## Ruling 2026-10-15: adapter for env-free sources only
Accept the fewer-params bind when the source has no env: a non-capturing lambda value (`M = L;` where L captures
nothing) and a named function (`M = someFunc;`) get the padded adapter (T189 padded-literal invoker). A CAPTURING source
stays refused unless spelled `M = move L;` (adapter takes the env over) - no implicit copy of a capture env
(lambda value-capture ruling 2026-09-30).
