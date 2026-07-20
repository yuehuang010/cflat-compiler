# `compile_error(<non-literal>)` silently emits a garbage message

Filed 2026-07-19. Found while consolidating duplicated diagnostics in `cflat/core/list.cb`.

## Summary

`compile_error()` requires a string LITERAL. Passing a named `const char*` constant is
accepted by the parser and still poisons the method, but the reported message is garbage -
a single stray character rather than the constant's text. No diagnostic warns that the
argument was not understood.

## Repro

```cflat
const char* MSG = "boom";
class B<T> { void f() { if const (!is_copyable(T)) compile_error(MSG); } };
class R { int i = default; };
class H { unique R* r = default; };
extern int main() { B<H> b = default; b.f(); return 0; }
```

```
ce.cb(5,38): S
```

Expected either `ce.cb(5,38): boom`, or a compile error saying `compile_error` needs a
string literal. Got neither - just `S`.

## Why it matters

It blocks the obvious de-duplication of poison messages. `cflat/core/list.cb` states the
same "element owns a unique pointer and has no copy()" rule at five sites; the natural fix
is one shared constant, but that silently produces `S` for every user who trips the rule.
The workaround in place is byte-identical literals repeated at each site, which stops the
wording from drifting but leaves the duplication.

Message drift caused by exactly this duplication is not hypothetical: before 2026-07-19 the
`set()` variant told callers to "write 'move' at the call site" when `list` had no
`set(int, move T)` overload at all, so the advice was unfollowable.

## Fix direction

Either evaluate a constant-folded `const char*` argument properly, or reject a non-literal
argument with a clear message ("compile_error requires a string literal"). Rejecting is
sufficient and is the smaller change; silently emitting a wrong message is the part that
must not stand.

## Related

- `cflat/core/list.cb` - the five duplicated literals this forces.
