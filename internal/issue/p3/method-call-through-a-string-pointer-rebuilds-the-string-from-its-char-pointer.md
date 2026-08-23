# A method call through a `string*` rebuilds the string from its data pointer via `strlen`

Filed 2026-08-22. Found while triaging an external report (cflat v0.11.0); not itself reported.
Reproduced on `master`, macOS arm64 Release.

Severity: low. Correct output for every string measured, but an O(n) `strlen` per call where an
O(1) field read is available, and the stored length / owned bit are discarded on the way.

## Repro

```cflat
import "string.cb";
int k1(string* p) { return p->length(); }
int k2(string* p) { return (*p).length(); }
int k3(string s)  { return s.length(); }
extern int main() { string s = "abc"; printf("%d %d %d\n", k1(&s), k2(&s), k3(s)); return 0; }
```

Prints `3 3 3` - the values are right. The emitted code is not what it should be:

```llvm
define internal i32 @_k1_i32_stringPtr_(ptr %p) {
entry:
  %0 = load ptr, ptr %p, align 8                                  ; loads FIELD 0 (the char*), not the string
  %1 = call %string @"_operator string_string_charPtr_"(ptr %0)   ; rebuilds a string: strlen(%0)
  %2 = call i32 @_length_i32_string_(%string %1)
  ret i32 %2
}
```

`_k2_` is byte-identical, so `->` and `(*p).` are the same path. For comparison, the by-value
receiver `k3` passes the loaded `%string` straight through with no conversion.

## What is going wrong

The receiver `*p` should be a whole-`%string` load off `%p`. Instead the pointer is treated as a
`char*` - field 0 is loaded and fed to the `char* -> string` conversion operator, which does
`strlen` and builds a **borrowing** string (raw length, owned bit clear). Consequences:

- `strlen` walks the buffer on every method call through a `string*`, instead of reading the
  stored `_len`.
- The stored length is discarded. A `string` whose length field disagrees with the first NUL in
  its buffer would report the `strlen` answer through a pointer and the stored answer by value.
  No such string was constructed for this file, so this is a latent divergence rather than a
  measured wrong answer - but the two spellings are no longer guaranteed to agree.
- The owned bit is dropped. Benign for a read-only method like `length()`, and it is what keeps
  this from being a double-free, but it means the receiver seen by the callee is not the caller's
  string.

`_operator string_string_charPtr_` itself is fine and does NOT leak - it stores the incoming
pointer and nulls its temp slot before returning, so nothing is allocated or freed.

## Fix direction

Where a method call's receiver is a deref of a pointer to an owning value type, load the whole
aggregate and pass it as `self` rather than falling through to the pointer-to-`char*` conversion.
The `string`-shaped conversion is presumably winning because a `string*` and a `char*` are the same
LLVM `ptr` after opaque pointers, so the receiver's declared `TypeName`/`Pointer` must drive this,
not the LLVM type.

## Acceptance

- `_k1_`/`_k2_` above emit a `load %string, ptr %p` and call `_length_i32_string_` directly, with
  no call to the `char*` conversion operator.
- `p->length()`, `(*p).length()` and the by-value `s.length()` still print `3 3 3`.
- Full suite green: `./test.sh Release` on macOS/Linux, `test.bat` on Windows. Add the three-way
  comparison to an existing string test file; do not create a new one.
