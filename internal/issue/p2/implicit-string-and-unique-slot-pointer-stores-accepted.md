# Implicit `string*` / `unique<NS>*` stores and a pointer-vs-array compare are accepted

Summary: after PTRSTORE (stores) and PRIMPTR (calls, `==` / `!=` / `<`), three neighbours of the
pointee rule are still accepted with no cast. Each is refused (or warned) by clang and, for the
first two, refused by the equivalent CFlat CALL, so stores are looser than calls here.

Repro (measured 2026-11-20 on fix/primptr, all compile and exit 7):
```
struct NS { int a = 7; int b = 0; };
struct NY { long z = 7; };
extern int main() {
    string s = "abc";
    char* q = &s;                    // string* -> char*: reads the string header as chars
    unique<NS> u = new NS();
    NS* r = &u;                      // unique<NS>* -> NS*: the owner slot address as an NS*
    NS[2] arr = default;
    NY y = default;
    NY* b = &y;
    return b == arr ? 1 : 7;         // NY* == NS[2]: distinct pointees compared
}
```

Measured cells (fix/primptr exe):

| shape | store / compare | call (`f(char*)`, `f(NS*)`) |
|---|---|---|
| `string*` into `char*` | ACCEPTED | refused ("no overload") |
| `unique<NS>*` into `NS*` | ACCEPTED | refused ("no overload") |
| `NY* == NS[2]` | ACCEPTED | n/a |
| `NY* == NS*`, `NS* == int*` | refused (PRIMPTR) | n/a |

clang oracle: `char* q = &s` with a struct `s` is -Wincompatible-pointer-types in C17 and an error in
C++20; `NY* == NS[2]` is "comparison of distinct pointer types" (warning C17, error C++20).

Root cause: IsImplicitUnrelatedStructPointeeConversion (cflat/LLVMBackend_Overloads.cpp) excludes
`string`, `string_view` and core unique types from its native-struct set on purpose (PTRSTORE): the
owner-slot address `&u` is a raw `T**` carrier elsewhere, and string views have their own rules. The
array compare reaches RejectImplicitPointeePointerStore (MainListener_BinaryOperators.cpp, the two
pointer-compare sites) with an array operand whose TypeName is empty after InferTernaryArmType, so
the predicate never sees the element name (hypothesis; not traced in a debugger).

Fix direction: decide (ruling) whether `&str` into `char*` and `&u` into `NS*` are stores the rule
refuses, then drop the exclusions for the store path only; for the compare, recover the array element
name the way UnrelatedPointeePair does (InferSourceTypeName for a fixed-array source).

Acceptance: each cell above gets a decision (refuse, or a recorded ruling); test.sh green.

- (TUPSTR review 2, pre-existing) a `string` declaration accepts ANY data pointer as its initializer and reads
  the pointee as text: `int v = 7; int* ip = &v; string s = ip;` compiles and `s.length()` is garbage; the same
  for `char**`, `unsigned char*`, `u8*`, `void*`, `string*`. Only a string literal or a `char*` / `const char*`
  should convert (the `(int, string)` tuple element already refuses the rest since TUPSTR).
