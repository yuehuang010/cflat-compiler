# `0xFFFFFFFF` is typed int (-1) in CFlat

Found by T63 review 2 (pre-existing CFlat literal typing).

## Repro
```cflat
std.vector<int> v{0xFFFFFFFF};   // cflat: accepted, element -1; clang++ -std=c++20: refused (narrowing:
                                 // 0xFFFFFFFF is unsigned int 4294967295)
int x = 0xFFFFFFFF;              // check native behaviour too
```
C rule: a hex literal that does not fit int takes unsigned int (then long, unsigned long, ...).

## Fix direction
Type hex/octal literals by the C ladder (int, unsigned int, long, unsigned long, long long,
unsigned long long). Needs a ruling if native CFlat code relies on `0xFFFFFFFF` being int -1.
