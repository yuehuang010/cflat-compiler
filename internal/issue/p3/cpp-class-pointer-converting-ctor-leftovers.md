# C++ class-pointer converting-constructor leftovers after T305

Round-1 T305 adds the C++ call-argument path for a C++ record pointer converting through a pointer-taking constructor. Four neighboring forms remain unresolved. All four repros behaved the same on the designated master executable and the round-1 worktree executable in the T305 round-2 session.

1. **Copy initialization:** `Foo foo; R r = &foo; return r.value + 30;` is refused by CFlat with `cannot initialize C++ class 'R'`; clang++ C++20 accepts it and returns 33. This is declaration initialization, outside the call-argument classifier.
2. **CFlat callee:** with `int cf(R r) { return r.value + 120; }`, both `cf(&foo)` and `cf(fp)` are refused as `no overload of 'cf'`; the equivalent C++ function returns 123. CFlat callees use a separate call path.
3. **Pointer-to-bool then user conversion:** `RBool(bool)` and a C++ `frb(RBool)` call from `Foo*` are refused as `no overload of 'frb'`; C++ converts the pointer to `true` and returns 46.
4. **Core name collision:** with `int take(R)` and `R(const Foo*)`, CFlat resolves `take(&foo)` against core `list.take` and reports no overload for `Foo*`; C++ selects `take(R)` and returns 53.

Minimal repro declarations: `Foo { int value = 3; }`, `R { R(const Foo* p) : value(p ? p->value : 0) {} }`, `RBool { RBool(bool b) : value(b ? 46 : 47) {} }`, `cf(R r) { return r.value + 120; }`, `frb(RBool r) { return r.value; }`, and `take(R r) { return r.value + 50; }`. The declaration probe is `R r = &foo`; the call probes are `cf(&foo)`, `cf(fp)`, `frb(fp)`, and `take(&foo)`. A clang++ C++20 oracle checked values 33, 123, 46, and 53.

Fix direction: assess each entry independently. Copy-init and the CFlat callee use different paths. `RBool` reaches C++ call-argument classification but needs pointer-to-bool followed by a user conversion. The `take` collision requires investigating core/C++ name lookup. Do not broaden the T305 pointer-constructor filter to cover these cases.

Found by: T305 round-2 review, 2026-10-16.
