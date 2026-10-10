# `(string)"abc"` is refused

Found by T36 round 5 (pre-existing on master).

## Repro
```cflat
extern int main() { string s = (string)"abc"; return (int)s.length(); }
```
cflat: "no overload of 'operator string'". Expected 3 - `(string)` on a `char*` works, and a string
literal is a `char*` everywhere (char literal ruling 2026-09-20); `string s = "abc";` also works.

## Fix direction
The explicit-cast operator-T lookup sees the literal's type before decay (char array / literal
kind); decay it to char* before ranking the `operator string` overloads.
