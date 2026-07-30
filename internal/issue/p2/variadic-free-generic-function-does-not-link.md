# A variadic generic FREE function compiles but does not link

Filed 2026-07-30, found while building the corpus for
[[interface-issue-queue]] (landed design records). Independent of namespaces and of the generic
key-space work - it reproduces at global scope with no namespace anywhere.

Severity: link failure with a raw JIT symbol dump, not a clean diagnostic. The compiler accepts the
program and then cannot produce it.

## Repro

```cflat
import "test_helper.cb";
int countOf<T...>() { return sizeof(T); }
extern int main() { printf("variadic free fn: %d\n", countOf<int,int,int>()); return 0; }
```

```
JIT session error: Symbols not found: [ __countOf__int__int__int_countOf__int__int__int__ ]
v_variadic.cb(3,78): --run: could not find 'main': Failed to materialize symbols: { ... }
```

Note the mangled name is doubled - `__countOf__int__int__int_countOf__int__int__int__` - which is
the shape of a name built by concatenating the mangled generic name into a slot that already
contains it. That doubling is the most concrete lead.

## What works

**Struct-shaped variadic packs are fine.** `Test/test_generics.cb:1852` declares
`struct SizeChecker<T...>` and it passes. So the pack machinery itself works; only the FREE FUNCTION
form fails, which points at the function-template instantiation path rather than at pack expansion.

## Root cause direction

Not investigated. The doubled symbol suggests the declaration and the definition disagree about the
mangled name for a variadic function instantiation - one side mangles the pack into the name and the
other mangles the already-mangled name again. Verify against the emitted IR (`--out-lli`) before
acting.

## Test coverage

None. No `Test/` file exercises a variadic generic FREE function; the only variadic-pack coverage is
the struct form above. That absence is why this went unnoticed.

Per CLAUDE.md, once the root cause is found this wants a proper compile-time diagnostic rather than
a JIT symbol dump, even if the underlying feature is not made to work in the same change.

Related: [[interface-issue-queue]] (landed design records), [[interface-issue-queue]]
