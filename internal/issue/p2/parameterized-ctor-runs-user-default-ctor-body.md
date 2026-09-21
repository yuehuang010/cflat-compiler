Bucket: p2 (silent double side effect in native CFlat; needs a maintainer ruling before any fix)

# A parameterized constructor implicitly runs the USER default constructor's body first

Found 2026-09-20 by the bridge plan phase 0 twin audit (macOS arm64, Release, master 49014d6f).
First reported as "constructor temporary leaks"; that root cause was wrong - there is ONE
allocation and no leak. Native CFlat only; no C++ interop involved.

## Repro

```cflat
extern int printf(const char* f, ...);
int n_ctor = 0;
struct Own
{
    int* data = new int[3];
    Own()      { n_ctor++; }
    Own(int v) { n_ctor++; }
    ~Own()     { delete[] data; }
};
extern int main() { { Own a = Own(9); } printf("ctor=%d\n", n_ctor); return 0; }   // prints ctor=2
```

Remove `Own()` and it prints 1. C++ prints 1 for the same program.

## Root cause (measured)

`--symbol-dump-ir function:Own`: `_Own$Own$.1$int` begins with `call %Own @"_Own$Own$.0"()`.
The parameterized constructor obtains its field-initialized `this` by calling the default
constructor, and when the user wrote one, that call includes the user's BODY. With no user
default constructor the same call only runs the field initializers, which is the intent.

## Why it matters

Any side effect in a default constructor body (a counter, a registration, a lock, a log line)
happens once more than written for every `T(args)`. It is the only use-site row where the CFlat
twin and its C++ twin disagree on constructor counts (3 vs 2 in scratch/tw_construct.cb), and
ruling R1 says CFlat concepts map directly onto the C++ ones.

## Fix direction (needs ruling)

1. C++ semantics: split field initialization from the user default-ctor body; a parameterized
   constructor runs field initializers only. Delegation stays explicit if it is ever wanted.
2. Keep and document it as CFlat semantics ("the default constructor is every constructor's
   prelude"), and state it in doc/LANGUAGE.md.

Acceptance for (1): the repro prints 1; a leg in Test/test_cpp_interop_bridge.cb's twin harness
for the construct row (same generic body over TwinCf and cpptw.Twin, equal counts).
