# Self-pointing struct returned by value dangles (no in-place return construction)

**RULED 2026-10-25 (maintainer): C++ guaranteed-elision semantics, scope = every ctor- or dtor-bearing native struct.** A native struct with a user-written ctor body or a destructor is returned by value via sret into the caller's final slot and constructed IN PLACE by `new T()` (heap slot) so `&this->field` stored in the ctor survives. Plain data structs (no user ctor body, no dtor) keep the register / value return. Plan-sized rollout, not a point fix (T510 r3 crashed doing it globally: core unique<T> ctor, duplicate destructor, list.add ASAN) - write internal/plan/ first. Acceptance unchanged: repro prints d=1 c=14; test_move --asan -g clean; T498Res/T510Res may move back off the global pool.

Residue of T510 (p2 landed without it). RULING NEEDED before a fix: return ABI for native structs.

## Repro (master and after T510)

```cflat
extern int printf(char* f, ...);
int d = 0;
struct B { int value = 7; int* heap = nullptr; B() { heap = &value; } ~B() { d = d + 1; } };
B mk2() { return B(); }
int fRec(int* p, int n) { if (n == 0) return 0; return *p + fRec(p, n - 1); }
int main() { int c = fRec(mk2().heap, 2); printf("d=%d c=%d\n", d, c); return 0; }
```

clang++ -std=c++20 twin prints d=1 c=14. cflat prints garbage for c (d=1). `fBox(mk2().heap)` happens to
read 7 only because its frame does not overwrite the dead slot.

## Root cause

`B()` stores `heap = &value` into its own local alloca, then returns the aggregate by value (`load %B` /
`ret %B`, no sret). `mk2()` forwards it; the caller extracts `heap` from the copy, which points into the dead
`B()` frame. C++ guarantees prvalue elision and returns a class with a non-trivial destructor indirectly
(sret into the caller's slot), so `&value` is the final object's address.

## Fix direction / ruling question

Return dtor-bearing (or all non-trivial) native structs via in-place sret like C++. T510 r3 tried this
globally for raw-pointer-field structs: core unique<T> ctor crash, duplicate destructor, then an ASAN
heap-use-after-free in list.add (Test/test_move.cb --asan -g). Needs a ruling on the native return ABI
(scope: dtor-bearing only vs every struct with a user ctor) and a planned rollout, not a point fix.
Acceptance: the repro prints d=1 c=14; test_move --asan -g clean.

## `new T()` also copies (2026-10-25)

`new T()` constructs T in the constructor's frame and copies the value into the heap allocation, so a constructor that
stores `&this->field` leaves the heap object pointing into a dead frame (clang constructs in place). The T498Res/T510Res
test_move fixtures relied on this and read stale stack memory (passed on arm64 by luck, garbage on Windows); 2940475db
moved them to global pools. The same ruling (in-place construction for dtor-bearing / self-pointing structs) covers
`new T()`.
