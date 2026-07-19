# Language & Core Library Features

## Table of Contents

- [Types](#types)
  - [Primitives](#primitives)
  - [Explicit-Width Integers](#explicit-width-integers)
  - [`string`](#string)
    - [String interpolation](#string-interpolation)
  - [Structs](#structs)
  - [Enums](#enums)
  - [`= default` Initialization](#-default-initialization)
  - [Brace Initializers](#brace-initializers)
- [Functions & Core Collections](#functions--core-collections)
  - [Overloading](#overloading)
  - [Default Parameters](#default-parameters)
  - [Named Parameters](#named-parameters)
  - [Returning Multiple Results](#returning-multiple-results)
  - [Forward References](#forward-references)
  - [Inline Return-Block Functions](#inline-return-block-functions-experimental)
  - [Function Pointers](#function-pointers)
  - [Lambdas](#lambdas)
- [Generics/Templates & Ownership](#genericstemplates--ownership)
  - [Generic Structs](#generic-structs)
  - [Generic Functions](#generic-functions)
  - [Generic Type Constraints](#generic-type-constraints)
  - [`is_pointer(T)` Intrinsic](#is_pointert-intrinsic)
- [Tuples](#tuples-coretuplecb)
  - [Tuple Type Syntax](#tuple-type-syntax)
  - [Tuple Construction](#tuple-construction)
  - [Destructuring Declaration](#destructuring-declaration)
  - [`ITuple<T...>` Interface](#ituplet-interface)
- [Interfaces & Polymorphism](#interfaces--polymorphism-coreinterfacescb)
  - [Defining and Implementing Interfaces](#defining-and-implementing-interfaces)
  - [Interface Parameters (VTable Dispatch)](#interface-parameters-vtable-dispatch)
  - [Generic Interfaces](#generic-interfaces)
  - [Interface Inheritance](#interface-inheritance)
  - [`is` and `as` Operators](#is-and-as-operators)
  - [Switch on Interface Type](#switch-on-interface-type)
- [Ownership / Lifetime (`move`)](#ownership--lifetime-move-keyword)
  - [`move` Return Type](#move-return-type)
  - [`alias` Return Type (borrow)](#alias-return-type-borrow)
  - [`bond` Lifetime Keyword](#bond-lifetime-keyword)
- [Namespaces & Modules](#namespaces--modules)
  - [Namespaces](#namespaces)
  - [`using` Aliases](#using-aliases)
  - [Module Imports](#module-imports)
- [Type Casting](#type-casting)
- [Null-Safety](#null-safety)
  - [Null-Conditional Access (`?.`)](#null-conditional-access-)
  - [Null-Coalescing (`??`)](#null-coalescing-)
- [Type Introspection](#type-introspection)
  - [`typeof`](#typeof)
  - [`nameof`](#nameof)
  - [`sizeof` / `alignof`](#sizeof--alignof)
- [Annotations](#annotations)
- [Reflection](#reflection)
  - [`IReflector` Interface](#ireflector-interface)
  - [`reflect(value, visitor)`](#reflectvalue-visitor)
- [Range-Based For](#range-based-for)
- [Control Flow](#control-flow)
  - [Ternary Operator (`? :`)](#ternary-operator--)
  - [`break` and `continue`](#break-and-continue)
  - [Pointer Arithmetic](#pointer-arithmetic)
- [Other Features](#other-features)
  - [Operator Overloading](#operator-overloading)
  - [Bitwise and Shift Operators](#bitwise-and-shift-operators)
  - [Numeric Literals](#numeric-literals)
  - [Built-in Identifiers](#built-in-identifiers)
  - [Reserved Keywords and Intrinsics](#reserved-keywords-and-intrinsics)
  - [Compile-Time Conditionals (`if const`)](#compile-time-conditionals-if-const)
- [`program` Keyword](#program-keyword)
- [C Interop](#c-interop) ([full reference](C_INTEROP.md))
- [WinMD / COM](#winmd--com) ([full reference](WINMD.md))
- [JSON](#json-libjsoncb)
- [Debug Info](#debug-info)
- [Compiler CLI](#compiler-cli) ([full reference](CLI.md))
- [Standard Library](#standard-library)
- [Threading & Memory Management](THREADING.md)
- [HPC & SIMD (`vectorize`, `simd<T,N>`)](HPC.md)

---

## Types

### Primitives

Standard C primitives are all supported: `int`, `char`, `short`, `long`, `float`, `double`, `bool`, `void`.

`long` follows the native C ABI of the **target** platform, exactly as a C compiler would:
32-bit on Windows (LLP64) and on 32-bit targets, 64-bit on macOS/Linux (LP64). `ulong` is its
unsigned counterpart. `int` is always 32-bit and `long long` is always 64-bit on every target.
Use `i64`/`u64` when a fixed 64-bit width is wanted regardless of target; use `long`/`ulong`
only to match a C API that is declared with C's `long` (for example `strtol`, `labs`).

### Explicit-Width Integers

CFlat provides fixed-width integer types as first-class names:

| Signed | Unsigned | Width  |
|--------|----------|--------|
| `i8`   | `u8`     | 8-bit  |
| `i16`  | `u16`    | 16-bit |
| `i32`  | `u32`    | 32-bit |
| `i64`  | `u64`    | 64-bit |

```c
i8  s8    = 100;
u32 flags = 0xFFFFFFFF;
i64 bigNum = 200000;
```

`i32` and `int` are the same type and are freely interchangeable.

### `string`

`string` is a built-in value type with layout `{ i8* _ptr, i32 _len }`. String literals are automatically wrapped into a `string` when assigned to a `string` variable or passed to a `string` parameter.

```c
string s = "hello";
s.length();   // 5
s.data();     // i8* - null-terminated bytes
```

Concatenation with `operator+` - produces a freshly allocated buffer:

```c
string a = "hello";
string b = a + " world";          // string + const char*
string c = a + b;                 // string + string
```

Any type implementing the `IString` interface participates in concatenation via its `ToString()` method:

```c
class Greeting : IString
{
    string ToString() { return "world"; }
};

Greeting g;
string result = "hello " + g;     // "hello world"
```

For incremental building, use `stringbuilder` - a mutable, growable buffer with `append`, `appendCStr`, `appendChar`, `clear`, and `toString`.

The standard library provides a broad set of string operations. A few examples to show the flavor:

```c
// Searching and testing
s.indexOf("lo");          // 3, or -1 if absent
s.startsWith("hel");      // true
s.contains("ell");        // true
bool ordered = s < t;     // lexicographic order

// Allocating transforms - result is owned, freed at scope exit
string upper = s.toUpper();
string trimmed = s.trim();
string part = s.substring(1, 3);
string fixed = s.replace("ell", "ELL");

// Split and join
list<string> parts = "a,b,c".split(",");
string joined = join("-", parts);         // "a-b-c"
```

Use LSP hover or `cflat.exe --symbol <name>` to browse the full API surface.

> **`charAt` clamps, does not signal EOF.** `s.charAt(i)` clamps `i` to
> `[0, length-1]` and returns the last character when `i >= length`. It cannot
> serve as an EOF sentinel. Always guard with an explicit bounds check:
>
> ```c
> // WRONG - charAt(len) returns last char, not 0
> while (s.charAt(pos) != '\n') pos++;
>
> // Correct
> while (pos < s.length() && s.charAt(pos) != '\n') pos++;
> ```
>
> For an empty string, `charAt` returns `0` regardless of the index.

Stringify a primitive explicitly with `value.toString()` (and `value.toString(base)` for an integer radix). A bare `(string)value` cast on a primitive is rejected - this keeps the owning heap allocation visible and matches C#/Java/Rust/Go:

```c
i64 n = 42;
string s = n.toString();        // "42"
string hex = n.toString(16);    // "2a"
double d = 3.14;
string sd = d.toString();       // "3.14"
```

For your own types, implement `IString` (`string ToString()`); string interpolation and concatenation pick it up automatically.

#### `char*` and `string`: the c-string boundary

`char*` (a raw C string) and `string` are distinct types that meet under a deliberate set of rules.

**Coercion is one-way.** `char*` -> `string` is implicit everywhere - call arguments, `==`, `+`, declaration initializers - by wrapping the pointer in a non-owning `string` (the `_len` comes from `strlen`). `string` -> `char*` is *never* implicit; call `.data()` to hand a `string` to a C API. The asymmetry is intentional: an implicit outbound decay would silently hand off a borrow whose lifetime matters, exactly where it matters most (extern C calls).

```c
extern void takesCStr(char* s);   // a C function
string s = "hello";
takesCStr(s.data());              // explicit - the borrow is visible
```

**Wrapping a `char*` borrows; it does not copy.** `(string)charPtr` (and the implicit coercion) produce a non-owning `string` that aliases the original buffer - it is *not* freed when that `string` goes out of scope, and it must not outlive the buffer it points at. To take ownership of an independent copy, call `.copy()`:

```c
string borrowed = (string)cptr;       // aliases cptr's bytes; do not outlive cptr
string owned     = ((string)cptr).copy();  // independent heap copy, freed at scope exit
```

Moving a borrowed `string` into a container is safe: `list<string>::add` / `dictionary::set` take `move string` and heap-copy a non-owning argument at the call site, so the container owns its own copy and freeing it never touches the borrowed buffer.

**`char* == char*` compares pointers, not content.** A `char*` is a raw C pointer, so `==` is pointer identity (`p == nullptr` and identity checks work as in C). Note that the compiler interns identical string literals to a single pointer, so two `"hello"` literals can *look* like a content compare while two heap buffers with the same bytes do not. To compare content, wrap one side so the `string` overload (`memcmp` over `_len`) runs, or use `strcmp`:

```c
bool sameContent = (string)a == b;    // value-compares the bytes
```

`string == string` is byte-exact over the tracked length, so it correctly handles embedded NUL bytes (unlike `strcmp`).

**`char* + char*` concatenates.** Pointer + pointer has no meaning in C, so `+` on two c-strings allocates a fresh owned buffer and concatenates (the caller frees it). `ptr + int` remains pointer arithmetic.

```c
char* a = "foo";
char* b = "bar";
string r = a + b;        // "foobar" - owned, freed at scope exit
```

**The cast rule, unified.** `(string)charPtr` is legal because it is a non-allocating borrow (no hidden cost). `(string)primitive` is rejected because it would hide a heap allocation - use `value.toString()` so the allocation is visible. The two rules are one principle: a `string` cast is allowed exactly when it does not allocate.

#### String interpolation

Any string literal containing `{expr}` is automatically treated as an interpolated string - no prefix required. Each `{expr}` segment is converted to `string` (primitives via their built-in stringification, custom types via `IString`) and the segments are concatenated at runtime:

```c
string name = "World";
string greeting = "Hello {name}!";   // "Hello World!"

int n = 42;
string msg = "n={n}";               // "n=42"

string a = "foo";
string b = "bar";
string both = "{a} and {b}";        // "foo and bar"
```

Any `IString` implementor works inside `{}`:

```c
class Point : IString
{
    int x = 0; int y = 0;
    string ToString() { return "{x}, {y}"; }   // nested interpolation
};

Point p; p.x = 10; p.y = 20;
string s = "Point is {p}.";   // "Point is 10, 20."
```

**Brace escaping** - use `{{` or `\{` for a literal `{`, and `}}` or `\}` for a literal `}`:

```c
string s = "value is {{x}} = {x}";   // "value is {x} = 42"  (x == 42)
```

> **Any `{` in a string literal triggers interpolation.** This is a common surprise
> when embedding JSON, format strings, or code templates. When a `{` is encountered
> but the content inside is not a valid expression, the compiler reports "Undefined
> variable" at line 1 of the file - not at the brace in question.
>
> Escape all literal braces that are not meant as interpolation anchors:
>
> ```c
> // BROKEN - compiler sees {value} as an interpolation, reports "Undefined variable 'value'"
> string tpl = "{"key": "{value}"}";
>
> // Correct - all structural braces escaped
> string tpl = "{{\"key\": \"{value}\"}}";   // {"key": "<content of variable 'value'>"}
> ```

### Structs

Structs support field defaults, member functions, constructors, and destructors:

```c
struct MyStruct
{
    int num1 = 1;
    int num2 = 2;

    int Total()
    {
        return num1 + num2;
    }

    ~MyStruct()
    {
        // runs when instance goes out of scope
    }
};

MyStruct s = MyStruct();
s.num1 = 10;
int t = s.Total();   // 12
```

#### Member access: `.` and `->`

Use `.` on a value and `->` on a pointer. Both reach **fields and methods** - `->`
dispatches a method through a pointer exactly as `.` does on a value:

```c
MyStruct  s = MyStruct();
MyStruct* p = &s;

int a = s.Total();    // value: dot
int b = p->Total();   // pointer: arrow - same method, dispatched through p
p->num1 = 10;         // field through a pointer
```

`->` is kept as a deliberate convenience for porting C code; it is not required (CFlat could
infer member access from the operand type), but it lets C/C++ source move over unchanged. It
is the natural spelling once a kernel passes large aggregates by pointer - e.g. a
`CsrMatrix* a` argument calling `a->spmv(x, y, n)`.


### Enums

Enums have an explicit backing type and their members are accessed with dot notation:

```c
enum Color : u8 { Red = 1, Green, Blue = 5 };
u8 c = Color.Red;    // 1
u8 b = Color.Blue;   // 5
```

### `= default` Initialization

The `default` keyword initializes primitives to zero and structs to their declared field defaults:

```c
int      n = default;   // 0
bool     b = default;   // false
MyStruct s = default;   // fields get their declared initial values
```

A struct field that has **no declared initializer** is zero-filled (0 / `false` / `nullptr`), so `= default` always produces a fully-initialized value - no field is left holding stack garbage:

```c
struct Mixed { int a = 7; int b; int c = 9; int d; };

Mixed m = default;      // a = 7, b = 0, c = 9, d = 0
Mixed z = {};           // same - `= {}` is the empty-brace spelling of value-init
```

The recommended style is still to give every field its own `= default` at the declaration (the standard library does this throughout), but a field that slips through is zeroed rather than left undefined.

---

### Brace Initializers

A brace list `{ ... }` initializes structs, fixed-size arrays, length-inferred array-views, and
the generic containers `list<T>` / `array<T>` / `dictionary<K,V>`. The element form depends on the
target type.

**Structs - named fields** (`field = value`, any order; omitted fields keep their declared defaults):

```c
struct Point { int x = default; int y = default; };

Point p = {x = 1, y = 2};
Point q = {y = 5};          // x stays at its default (0)
```

A *positional* list on a struct (`Point p = {1, 2}`) is a compile error - use named fields.

**Fixed-size arrays `T[N]` - positional** (the size belongs to the type). Fewer elements than `N`
leaves the trailing slots zero-filled; more than `N` is an error:

```c
int[5] a = {7, 8, 9};       // -> {7, 8, 9, 0, 0}
int[3] b = {100, 200, 300};
char[4] c = {72, 73, 0, 0}; // 'H', 'I'
```

**Length-inferred array-view `T[]` - positional.** `T[]` is a thin `T*`; the brace list both
supplies the backing storage and infers its length. An empty `{}` is an error (nothing to infer
from); use an explicit `T[N]` for a zero-length-but-sized array. Note: `T[]*` and `T[N]*`
(pointer-to-array-view / pointer-to-fixed-array) are not valid types - to return several
results, return one `T[]` and pass extra arrays as `T[]` out-parameters and scalars as `T*`
(see [Returning Multiple Results](#returning-multiple-results) for the pattern and the noalias
guarantee):

```c
int[]    v = {11, 22, 33};  // backing [3 x int]; v points at element 0
string[] s = {"x", "y"};    // char* literals coerced to string
```

**Containers - positional for `list`/`array`, `key: value` for `dictionary`:**

```c
import "list.cb";
import "array.cb";
import "dictionary.cb";

list<int>            li = {10, 20, 30};          // -> add() per element
list<string>         ls = {"a", "b", "c"};       // char* literals coerced to string
array<int>           ai = {1, 2, 3, 4};          // -> init(4) then set(i, v)
dictionary<int,int>  di = {1: 100, 2: 200};      // -> set(k, v) per pair
dictionary<string,string> ds = {"k": "v"};
```

An empty `{}` is valid for a container (it just yields an empty one) - unlike `T[]`, the type
carries no length to infer.

> **Element types and conversions.** Elements follow CFlat's normal conversion rules: widening
> (e.g. `int` -> `i64`) is implicit, but there is **no implicit down-conversion**. The container
> paths resolve `add`/`set` by overload, so a narrowing element literal does not match -
> `list<float> = {1.5}` (the literal `1.5` is `double`) and `list<u8> = {255}` are rejected.
> `char*` literals are the one special case: they are coerced to `string` for `string` elements.

---


## Functions & Core Collections

### Overloading

Multiple functions can share a name and are resolved by argument types.
```cpp
bool Test(const char* name, float actual, float expected) { ... }
bool Test(const char* name, bool actual,  bool expected)  { ... }
```

### Default Parameters

Parameters can have default values; the compiler generates the matching overloads automatically.
```
sum(1, 2);  // 23  (z=20)
```


### Named Parameters

Arguments can be passed by name, in any order:

```c
int sum(int x, int y, int z) { return x + y + z; }

int a = sum(x: 1, y: 2, z: 3);         // in order
int b = sum(z: 3, x: 1, y: 2);         // out of order
int c = sum(x: 1, y: 2, 3);            // mixed with positional
```

### Returning Multiple Results

A function returns a single value, so to hand back several results, return one and pass the
rest as **out-parameters**. The sanctioned pattern: return one `T[]`, pass any extra arrays as
`T[]` out-parameters, and pass scalar outputs as `T*` out-parameters.

```c
// Returns the summed array; also reports how many elements it touched via an int* out-param.
void compute(double[] a, double[] b, double[] outResult, int* outSteps)
{
    int i = 0;
    while (i < 1024) { outResult[i] = a[i] + b[i]; i = i + 1; }
    *outSteps = 1024;
}
```

Each `T[]` parameter carries a **noalias** contract - distinct `T[]` values address distinct
whole allocations, so the compiler stamps the `noalias` attribute on every `T[]` argument
(each also gets its own alias scope). A scalar `T*` out-param does **not** carry noalias, which
is correct: it points at a single cell, not a whole buffer. The emitted IR shows the
distinction:

```
define internal void @_compute_..._(ptr noalias %a, ptr noalias %b, ptr noalias %outResult, ptr %outSteps)
```

This noalias guarantee is a general property of `T[]` (it lets the optimizer reason about
disjointness without runtime overlap checks - see [HPC.md](HPC.md) for how the loop vectorizer
exploits it), not something specific to high-performance code.

`T[]*` (pointer-to-array-view) and `T[N]*` (pointer-to-fixed-array) are **not valid types**;
the compiler reports an error pointing at this pattern. Use the out-parameter form above
instead.

#### Returning a tuple of array-views

When returning several arrays is cleaner than out-parameters, a tuple element may itself be a
`T[]` array-view: `(T[], T[])` (sugar) or the explicit `tuple<T[], T[]>`. Each element keeps the
array-view noalias contract, so destructuring the result into locals yields views the optimizer
can still prove disjoint (each destructured local gets its own alias scope).

```c
// Split a buffer into two views and return both at once.
(int[], int[]) halves(int[] a, int n)
{
    int[] lo = a;            // (illustrative - real code would slice)
    int[] hi = a;
    return (lo, hi);
}

(int[] x, int[] y) = halves(view, n);   // x and y are noalias array-views
```

Only the bare `T[]` form is allowed as a tuple element or generic type argument; a sized
`T[N]` or a pointer-to-array `T[]*` is rejected with a clear message (use `T[]`, or the
out-parameter pattern above).

### Forward References

Functions can be called before they are defined - no forward declarations needed. Mutual recursion works out of the box:

```c
bool isEven(int n)
{
    if (n == 0) return true;
    return isOdd(n - 1);    // isOdd defined below
}

bool isOdd(int n)
{
    if (n == 0) return false;
    return isEven(n - 1);
}
```

### Inline Return-Block Functions (Experimental)

A function whose body is a single `return { ... };` is inlined at every call site. The `return` statement inside the block exits the **caller**, not the function. This enables assert-style helpers:

```c
inline bool Assert(const char* name, int actual, int expected)
{
    return {
        if (actual != expected)
        {
            printf("%s failed.\n", name);
            return false;   // returns false from the function that called Assert
        }
    };
}

bool runTest()
{
    Assert("check1", compute(), 42);  // exits runTest early if it fails
    Assert("check2", other(),   10);
    return true;
}
```

### Function Pointers

CFlat has two callable types - choose by whether the callable must capture state:

| Type | Layout | Captures? | Use it for |
|------|--------|-----------|-----------|
| `function<R(Args)>` | thin: an 8-byte bare C function pointer `R(*)(Args)` | No | C-ABI callbacks and non-capturing callables. This is the type you hand to real C APIs. |
| `Lambda<R(Args)>` | fat: a 16-byte owning closure `{code, env}` | Yes | Capturing lambdas and stored callbacks that close over state. An owning value type (moves by default - see [Ownership](#ownership--lifetime)). |

Use `function<ReturnType(ParamTypes...)>` for a thin function pointer. The bare `function` keyword infers the type from the assigned function:

```c
int add(int a, int b) { return a + b; }
int mul(int a, int b) { return a * b; }

function<int(int, int)> f = add;
f(3, 4);    // 7

f = mul;
f(3, 4);    // 12

function g = add;   // inferred type
g(1, 2);            // 3
```

Function pointers can be passed as arguments:

```c
int apply(function<int(int, int)> f, int a, int b) { return f(a, b); }

apply(add, 5, 6);   // 11
```

**Converting between the two:**

- A thin `function<T>` converts implicitly to a fat `Lambda<T>` - the bare pointer is stored with a null environment, so no captured state is added.
- A `Lambda<T>` does **not** convert implicitly to `function<T>` (a closure that captures has no C-ABI representation). Use the explicit member `fn.toFunction()`, which returns the bare code pointer when the closure does not capture, or `nullptr` when it does - the caller null-checks:

```c
Lambda<int(int)> add1 = makeAdder(1);            // may or may not capture
function<int(int)> c = add1.toFunction();      // bare ptr if non-capturing, else nullptr
if (c != nullptr) { passToCApi(c); }
```

> **Passing a function to C code.** A `function<T>` *is* a bare C function pointer, so a non-capturing named function or non-capturing lambda passes directly to a C function-pointer parameter (`qsort`, a Win32 `WNDPROC`, ...). A capturing closure (`Lambda<T>`) cannot cross the C ABI; convert it with `.toFunction()` and null-check, or pass state through a `void* userdata` channel. Win32 callbacks should also be marked `stdcall`. See [C Interop: passing a CFlat function as a C callback](C_INTEROP.md#passing-a-cflat-function-as-a-c-callback).

### Lambdas

Anonymous functions use C#-style arrow syntax. Capture is automatic - the lambda body is scanned at compile time and outer variables are captured without an explicit capture list. **The target type decides the representation:** a non-capturing lambda can become either a thin `function<T>` or a fat `Lambda<T>`; a **capturing** lambda must become a `Lambda<T>` (assigning one to a thin `function<T>` is a compile error, since a C function pointer cannot carry captured state).

**Non-capturing lambda** - captures nothing; works identically to a named function and fits a thin `function<T>`:

```c
function<int(int, int)> add = (int a, int b) => { return a + b; };
add(3, 4);   // 7

// Passed inline as an argument
apply((int a, int b) => { return a * b; }, 5, 6);   // 30

// Zero-parameter
function<int()> getK = () => { return 42; };

// Full control flow inside the body
function<int(int)> sign = (int x) => {
    if (x > 0) return 1;
    if (x < 0) return -1;
    return 0;
};
```

**Value capture** - primitives, pointer types, and `string` are copied into the closure at lambda creation time. A capturing lambda is a `Lambda<T>`. Subsequent changes to the outer variable do not affect the captured copy:

```c
int offset = 10;
Lambda<int(int)> addOffset = (int x) => { return x + offset; };
addOffset(5);   // 15
offset = 99;    // does NOT change the captured copy
addOffset(5);   // still 15
```

Pointer captures copy the pointer value - both the lambda and the outer code share the same pointed-to memory:

```c
int* p = new int;
*p = 42;
Lambda<int()> read = () => { return *p; };
*p = 77;
read();   // 77 - both sides access the same heap cell
```

**Reference capture** - non-pointer struct and class values are captured by reference. The lambda holds a pointer to the outer variable; mutations on either side are immediately visible to the other:

```c
struct Counter { int value = 0; };

Counter c;
Lambda<void()> inc = () => { c.value = c.value + 1; };
inc(); inc(); inc();
c.value;   // 3 - lambda mutations are visible in the caller

c.value = 10;   // outer mutation
inc();
c.value;   // 11 - outer mutation was visible inside the lambda
```

**Lifetime constraint** - a lambda that reference-captures a local variable is *bonded* to it. The lambda cannot be returned from the function or assigned to a variable in a wider scope, because doing so would leave the lambda holding a dangling pointer to a stack variable that has gone out of scope:

```c
Lambda<void()> bad()
{
    Counter c;
    Lambda<void()> inc = () => { c.value = c.value + 1; };
    return inc;   // compile error: bonded value whose source 'c' is not a 'bond' parameter
}
```

The lambda can be freely passed to functions called within the same scope - only escape (return or wider-scope assignment) is prevented.

---


## Generics/Templates & Ownership

Generics are resolved at compile time (monomorphization - no runtime overhead).

### Generic Structs

```c
struct Box<T>
{
    T value = default;
};

struct Pair<K, V>
{
    K first  = default;
    V second = default;
};

Box<int> intBox = default;
intBox.value = 42;

Pair<int, float> p = default;
p.first  = 1;
p.second = 3.14f;
```

### Generic Functions

```c
T ExtGet<T>(Container<T> container)
{
    return container.Get();
}
```

### Generic Type Constraints

Use `where T : IInterface` to require that a type parameter implements an interface. The constraint is checked at the instantiation site:

```c
T maxOf<T>(T a, T b) where T : IComparable<T>
{
    return a.CompareTo(b) >= 0 ? a : b;
}
```

### `is_pointer(T)` Intrinsic

`is_pointer(T)` is a compile-time intrinsic that returns `1` if `T` resolves to a pointer type in the current generic instantiation, `0` otherwise. Combine with `if const` to branch on element ownership:

```c
void cleanup<T>(T elem)
{
    if const (is_pointer(T))
    {
        delete elem;   // only compiled for pointer instantiations
    }
}
```

---


## Tuples (`core/tuple.cb`)

Tuples are heterogeneous fixed-length sequences. The underlying type is `tuple<T...>` (a variadic generic struct), but CFlat provides concise syntax for all common operations.

### Tuple Type Syntax

`(T1, T2, ...)` is syntactic sugar for `tuple<T1, T2, ...>`. Use it anywhere a type is expected:

```c
(int, float) point = default;
point.item_0 = 3;
point.item_1 = 1.5f;

int n = point.size();   // 2 - number of elements
```

Fields are named `item_0`, `item_1`, ... and can be accessed directly.

### Tuple Construction

A parenthesized expression list `(expr1, expr2, ...)` (two or more elements) constructs a tuple by value. The type is inferred from the elements:

```c
(int, float) p = (3, 1.5f);        // tuple<int, float>
(int, float, bool) t = (42, 2.7f, true);
```

Tuples can be returned from functions:

```c
(int, float) makePoint()
{
    return (3, 1.5f);
}
```

### Destructuring Declaration

A tuple value can be unpacked into named local variables in a single declaration:

```c
(int x, float y) = makePoint();
// x == 3, y == 1.5f

(int a, float b, bool c) = makeTriple();
```

The right-hand side can be any expression that evaluates to a matching tuple type - a variable, a function call, or a construction expression.

#### Inferred entries and the `_` wildcard

An entry may omit its type to have it inferred from the corresponding tuple field, and `_`
discards a slot (no variable is bound; the value stays in the tuple and is freed with it):

```c
(int a, b)   = makePoint();   // b inferred as float
(int a, _)   = makePoint();   // second slot discarded
(_, float b) = makePoint();   // first slot discarded
```

At least one entry must carry an explicit type. A fully type-less `(a, b) = expr;` is
indistinguishable from an ordinary assignment expression, so it is parsed as one (an assignment
to existing variables), not a destructure.

To destructure with no types at all, prefix the pattern with `auto` - it disambiguates from an
assignment expression, so every entry may be inferred or `_`:

```c
auto (a, b)    = makePoint();   // both inferred
auto (x, _, z) = makeTriple();  // middle slot discarded
```

### `ITuple<T...>` Interface

`ITuple<T...>` (in `interfaces.cb`) is a contract for types that can expose their contents as a tuple. Implement it to make a custom type destructurable via its `get()` method:

```c
import "interfaces.cb";

class MyRange : ITuple<int, int>
{
    int lo = 0;
    int hi = 0;

    (int, int) get()
    {
        return (lo, hi);
    }
};

MyRange r;
r.lo = 5;
r.hi = 10;

(int a, int b) = r.get();   // a == 5, b == 10
```

The interface definition is:

```c
interface ITuple<T...>
{
    (T...) get();
};
```

---


## Interfaces & Polymorphism (core/interfaces.cb)

### Defining and Implementing Interfaces

```c
interface IReadable
{
    int Read();
};

interface IScalable
{
    int Scale(int factor);
};

class Counter : IReadable
{
    int count = 10;
    int Read() { return count; }
};

// A class can implement multiple interfaces
class ScaledValue : IReadable, IScalable
{
    int value = 3;
    int Read()            { return value; }
    int Scale(int factor) { return value * factor; }
};
```

> **`class` is required for interface implementation.** Only `class` definitions
> support the `: IFace` base list. A bare `struct` cannot carry an interface list and
> the parser rejects it. Use `struct` for plain data aggregates with no interface
> contracts; use `class` when you need VTable dispatch.

### Interface Parameters (VTable Dispatch)

A function accepting an interface type works with any implementing class or struct:

```c
int readValue(IReadable reader)
{
    return reader.Read();
}

Counter c = Counter();
readValue(c);   // dispatches via VTable
```

### Interface Fields

An interface may declare **fields** as well as methods. Every implementor must declare a
field of the same name and type; the check runs at the class definition. A field reached
through an interface value is a true lvalue - read it, write it, or write a member of a
nested struct through it:

```c
struct Frame { int width = 0; int height = 0; };

interface IElement
{
    int kind();
    int   tag;       // every implementor must have `int tag`
    Frame bounds;
};

class Button : IElement
{
    int   pad = 0;   // implementors may lay their fields out however they like
    int   tag = 0;
    Frame bounds = default;
    int kind() { return 1; }
};

IElement e = new Button();
e.tag = 7;               // write
e.bounds.width = 100;    // write through a nested struct field
int k = e.tag;           // read
```

There is no indirect call: each field costs one vtable slot holding that implementor's byte
offset of the field, so two implementors with completely different layouts both work. Fields
are inherited by a derived interface exactly like methods, including across several levels.

Reading or writing a field on a **null** interface value (the result of a failed
`as <Interface>`) faults, exactly as calling a method on one does. Guard with a null compare
(see below) before reaching through a value that may have missed.

### Generic Interfaces

Generic interfaces may carry fields too (`T value;` resolves to the substituted type).

```c
interface Container<T>
{
    T Get();
    void Set(T value);
};

class Storage<T> : Container<T>
{
    T data = default;
    T Get()           { return data; }
    void Set(T value) { data = value; }
};

Storage<int> s = default;
s.Set(42);
int v = s.Get();   // 42
```

### Interface Inheritance

```c
interface IString : IReadonlyString
{
    void Append(const char* text);
};
```

A derived-interface value converts **implicitly** to any of its ancestor interfaces - on
assignment, as a call argument, when added to a `list<IAncestor>`, and on return:

```c
interface ITipped : IElement { string tip; };      // IElement as above
interface IPress  : ITipped  { string title; };
class Press : IPress { /* tag, bounds, tip, title, kind() */ };

int kindOf(IElement e) { return e.kind(); }

move IElement make()
{
    IPress b = new Press();
    return b;              // IPress -> IElement
}

IPress   b = new Press();
IElement e = b;            // implicit upcast (two levels)
kindOf(b);                 // implicit upcast at the call
```

The conversion rebuilds the fat pointer from the runtime type, so it is correct for
multi-level and multi-parent chains. An exact-interface overload still wins over one that
requires an upcast.

The upcast is purely a type conversion - it does not change ownership. Adding to
`list<IAncestor>` still borrows (never frees) unless the list's element type is
`list<unique IAncestor>`, exactly as for the exact interface type; see
[`unique` Ownership](#unique-ownership).

### `is` and `as` Operators

`is` tests the concrete runtime type of an interface value. It also accepts an **interface**
on the right, which is true when the runtime type implements it (at any depth):

```c
if (e is IPress) { ... }     // does the value behind `e` implement IPress?
```

An interface value can be compared against `nullptr` - a failed `as` yields a null value,
and this is the guard for it:

```c
IPress b = e as IPress;
if (b != nullptr) { printf("%s\n", b.title.data()); }
```

`as` downcasts to a concrete struct pointer or interface; returns `nullptr` if the cast fails:

```c
IShape sc = someCircle;

if (sc is Circle)   // true if sc holds a Circle
{
    Circle* cp = sc as Circle;   // non-null
    printf("radius %d\n", cp->r);
}

// as returns nullptr on mismatch
Square* sp = sc as Square;   // nullptr - sc is a Circle
```

`as` also casts between interface types:

```c
IDrawable d = sc as IDrawable;   // interface-to-interface
```

### Switch on Interface Type

`switch` dispatches on the concrete type held in an interface variable:

```c
IShape shape = getShape();

switch (shape)
{
    case Circle:   printf("circle\n");   break;
    case Square:   printf("square\n");   break;
    case Triangle: printf("triangle\n"); break;
    default:       printf("unknown\n");  break;
}
```

### Interface Value Semantics - Current Limitations

- **An interface value returned from a function must box a HEAP object.** An interface is a
  fat pointer (object ptr + vtable ptr), so boxing a stack local and returning it hands the
  caller a pointer into a dead frame. The direct form is rejected at compile time; the
  two-step form is not detected, so do not write it.

  ```c
  IShape getShape() { Circle c; return c; }              // ERROR - would dangle
  IShape getShape() { Circle c; IShape s = c; return s; } // DANGLES - not diagnosed

  move IShape getShape() { return new Circle(); }        // correct: heap object
  ```

  Boxing a heap pointer into an interface MOVES ownership into the interface value, and
  interface locals are not auto-destructed - the holder must `delete` it. Returning one from
  a non-`move` function is rejected (the caller could not see that it owns the object).

- **`interface*` (pointer-to-interface) is rejected.** You cannot take the address of an
  interface variable. Use a concrete pointer where an indirect reference is needed.

---


## Ownership / Lifetime (`move` keyword)

CFlat uses **context-based ownership**: a local variable owns the pointer it allocates, and a function parameter borrows by default. A struct field owns nothing unless you say so, with [`unique`](#unique-ownership). A **container** of pointers or interfaces (`list<T*>`, `list<IShape>`) borrows its elements by default too - it never frees them - unless the element type itself is marked `unique` (`list<unique T*>`, `list<unique IShape>`); see [`unique` Ownership](#unique-ownership) for the container form and its rules.

The `move` keyword on a parameter definition transfers ownership into the callee - the resource is freed when the function returns:

```c
void consume(move Resource* r)
{
    // r is owned here; freed on return
    printf("id=%d\n", r->id);
}

void borrow(Resource* r)
{
    // caller still owns r
    printf("id=%d\n", r->id);
}

Resource* res = new Resource();
consume(res);   // res is automatically nulled in the caller after this call
// res is nullptr here
```

- **Call site is silent** - no annotation required; the caller's pointer is automatically nulled after a `move` call.
- `move` is a soft keyword detected by text matching, not an ANTLR token - identifiers named `move` still work.
- For value types `move` is a no-op; ownership semantics only activate on pointer types.

### `move` Return Type

`move T*` on a function's return type declares that the caller receives ownership of the returned pointer - the caller is responsible for freeing it (or passing it along).

```c
// Caller receives ownership - pointer freed when r goes out of scope
move Resource* createResource(int id)
{
    Resource* r = new Resource();
    r->id = id;
    return r;   // OK: new-allocated, caller owns it
}

// Forward ownership through a move parameter
move Resource* forwardResource(move Resource* r)
{
    return r;   // OK: r is a move parameter - already owned
}

{
    Resource* r = createResource(7);
    // r is owned; freed at end of scope (destructor called once)
}
```

**Rules at return sites** - the compiler enforces that the returned expression is genuinely owned:

| Returned expression | Allowed? |
|---|---|
| Result of `new` | Yes |
| `move` parameter | Yes |
| Result of another `move T*`-returning call | Yes |
| Borrowed parameter (`Resource* r`) | **Error** |
| `nullptr` | Yes (no ownership transferred) |

```c
move Widget* leak(Widget* w)
{
    return w;   // compile error: returned expression is not owned
}
```

- **Local variable propagation**: assigning the result of a `move T*` call to a local variable transfers ownership - the local is freed at scope exit just like a `new`-allocated pointer.
- **Forwarding**: a `move T*` function can return a `move` parameter directly; the cleanup that would normally run at scope exit is suppressed so the resource isn't freed prematurely.

### `alias` Return Type (borrow)

`alias` is the dual of `move`. For an **owning struct value type** (a struct with a destructor, e.g. one holding a `string` field), there are three return forms:

| Return form | Meaning | Caller frees? |
|---|---|---|
| `move T` | by value, ownership transferred out | yes |
| `T` | by value, **owning** (default) | yes |
| `alias T` | by value, **borrow** - the source still owns the storage | **no** |

A plain by-value return is owning by default - the caller receives a value it must (and does) free. Use `alias` when the returned value only *borrows* storage that something else keeps owning, such as a collection accessor that hands back an element the collection still holds:

```c
struct list<T>
{
    // The element stays owned by the list, so the returned value must not be
    // freed by the caller. `alias` says so; the caller may read it, and uses
    // `.copy()` to take an independent owned copy.
    alias T get(int index) { return _data[index]; }
}
```

`alias` returns a **value**, not a pointer, so it does not reintroduce a dangling reference by itself. It only says "this value does not own its interior buffers." This lets an accessor stay cheap (no per-read copy) while keeping ownership honest.

**What the compiler does with an `alias` result:**

- **Unbound temp read** (`toks.get(0).text.length()`) - the borrow is read and never freed; the collection frees its element once. (A plain owning return in the same position is instead destructed at end-of-statement, so neither leaks nor double-frees.)
- **Bound to a local** (`Token t = toks.get(0);`) - `t` is marked a borrow; its scope-exit destructor is suppressed so it does not double-free the collection's buffer.
- **Escapes are compile errors.** An `alias` value cannot outlive its source, so the following are rejected (the fix is `.copy()`, which takes an independent owned value, or declaring the function itself `alias` to pass the borrow through):

```c
struct Holder { Token tok = default; };

Token first(list<Token> toks)   { return toks.get(0); }   // error: returning an 'alias' value
void  stash(list<Token> toks, Holder h) { h.tok = toks.get(0); }   // error: storing an 'alias' value into a field
```

**Notes:**

- `alias` is a soft keyword detected by text matching, not an ANTLR token - identifiers named `alias` still work.
- `alias` applies to **owning struct value types**. `string` (and closures) are excluded: they carry a runtime owned bit that already clears on a borrow return, so `return someList.get(i)` for a `string` is safe without `alias`.
- The discriminator for choosing `alias`: tag it when the source **still owns** the element after the call (a peek, like `list.get`). If the call **consumes/releases** the element (the source no longer owns it, like `channel.receive`/`pop` whose producer side is `push(move T)`), leave it owning.

### `bond` Lifetime Keyword

`bond` annotates a function parameter to declare that the return value borrows from it - the caller's variable cannot be replaced or freed while a bonded local exists that refers to it. This catches dangling-reference bugs at compile time with no annotation required at call sites:

```c
// The return value borrows from src; it must not outlive src.
int* Borrow(bond int* src) { return src; }

int a = 10;
int* view = Borrow(&a);   // view is bonded to a
```

**Rules enforced by the compiler:**

| Violation | Error |
|-----------|-------|
| Assign bonded value to a wider-scope variable | bonded value cannot be assigned to `x` - `x` is in a wider scope than its bond source |
| Store bonded value into a struct field or through a pointer | bonded value cannot be stored in a struct field or through a pointer |
| Reassign the bond source while the bond is live | cannot reassign `a` while `view` holds a bonded reference to it |
| Pass bonded value to a `move` parameter | bonded value cannot be passed to a `move` parameter |
| Return a bonded value whose source is a local (not a `bond` param) | returning bonded value whose source `x` is not a `bond` parameter |

**Breaking a bond** - assigning `null` (or `nullptr`) to the bonded variable clears the bond, after which the source may be freely reassigned:

```c
int a = 10;
int* view = Borrow(&a);
view = nullptr;   // bond cleared
a = 20;           // OK now
```

**Propagating a bond up the call chain** - a function that receives a `bond` parameter may return it, and the compiler tags the caller's local as bonded in turn:

```c
int* ForwardBorrow(bond int* src) { return src; }

int a = 10;
int* p = ForwardBorrow(&a);   // p is bonded to a
```

`bond` and `move` are mutually exclusive on the same parameter. `bond` is a soft keyword (text-matched at parse time), so identifiers and methods named `bond` still work in other contexts.

### `unique` Ownership

`unique` is a qualifier on a pointer or interface type. It is legal on a struct field
(the original position), a **local variable**, a **function parameter**, and a **generic
type argument** (`list<unique Circle*>`, `list<unique IShape>`). It is not legal on a
value type (`unique int`, `unique Circle` where `Circle` is not a pointer) - error
`unique requires a pointer or interface type; '<T>' is neither` - and it is not legal
in return position, as a tuple element type, or as an explicit generic-function type
argument (`identity<unique N*>(n)`); transfer out of a return is `move`.

At runtime `unique` is free - it is still just a pointer or a fat interface pointer, no
wrapper and no extra field. It only changes what the compiler statically requires and
enforces at the declaration site that carries it.

#### Field ownership (original form)

A raw pointer field is **not** owned by default - nothing frees it, so it leaks unless you write a destructor. `unique` marks the field as owning, and the compiler-synthesized destructor deletes the pointee for you:

```c
struct Tree
{
    unique Node* _root = nullptr;   // the synthesized destructor deletes _root
};
```

That replaces the hand-written form, which is what you would otherwise have to write:

```c
struct Tree
{
    Node* _root = nullptr;
    ~Tree() { if (_root != nullptr) delete _root; }
};
```

`unique` is **opt-in and additive**: an unmarked `T*` field behaves exactly as it always has. Mark only the fields your struct genuinely owns. It composes with `alignas(0, N)` - an over-aligned pointee is freed through the matching aligned path.

**Assigning to a `unique` field frees whatever it held.** There is no need to delete first, and no way to leak the old value:

```c
Tree t = default;
t._root = new Node();     // field was null: nothing to free
t._root = new Node();     // frees the FIRST node, then stores the new one
t._root = nullptr;        // frees it early - the release idiom
```

**Rules the compiler enforces:**

| Rule | Why |
|---|---|
| `delete _root;` is **rejected** | The synthesized destructor already deletes it - an explicit delete is a double-free. Assign `nullptr` instead; it frees the pointee. |
| Storing a **borrowed** parameter is **rejected** | The caller still owns it and frees it on scope exit, so the field would dangle. Declare the parameter `move` to transfer ownership. |
| A struct with a `unique` field has **no memberwise copy** | Copying would hand one pointer to two owners. Like `unique_ptr`, it is move-only: `move` it, or write your own `copy()` that clones the pointee. |
| A local aliasing a `unique` field cannot be `delete`d or `move`d | The alias does not null the field, so the field's destructor would still free the pointee. Use `move t._root` to move the **field** itself, which nulls it. |
| **Recursive** ownership is **rejected** | A `unique` field whose pointee type reaches a `unique` field of the same type (a tree or a linked list) would recurse without bound on teardown. There is no good general answer - the recursive teardown overflows on a degenerate shape, and an iterative one needs an allocation during destruction that can itself fail. A recursive data structure knows the shape of its own data, so its author writes the destructor. |
| **Fixed arrays** (`unique Node* kids[8];`) are **rejected** | The synthesized destructor deletes a single pointer and would leak the rest. |

**Taking ownership back out** of a `unique` field is what `move` is for - it nulls the field, so the destructor skips it:

```c
Node* n = move t._root;   // t._root is now nullptr; you own n
delete n;                 // your responsibility
```

`unique` is a soft keyword (text-matched at parse time), so identifiers named `unique` still work in other contexts.

#### Local ownership

A `unique` local is an **owning location**, exactly like a `unique` field: it must be
initialized from `new`, a `move` expression, a call that returns `move T*`, or `nullptr`.
Initializing it from a borrowed source is rejected - a borrowed value already has an
owner, and a second owning handle to the same pointer is a double-free waiting to happen:

```c
struct Resource { int id = 0; ~Resource() {} };

void takes(Resource* borrowed)
{
    unique Resource* u = borrowed;   // error: cannot initialize unique 'u' from a
                                      // borrowed value - the source still owns it;
                                      // use 'new', a 'move' expression, or a
                                      // move-returning call, or drop 'unique'
}

{
    unique Resource* a = new Resource();   // OK: freed once at scope exit
}
```

**Reading a `unique` local without `move` yields a borrow**, not a transfer - the
declared type stays unique, the storage stays owned by `a`, and there is no
double-free:

```c
unique Resource* b = new Resource();
Resource* view = b;        // borrow: `b` still owns the Resource
printf("%d\n", view->id);
// b is freed once at scope exit
```

`delete` on a `unique` local is rejected, the same as on a `unique` field - assign
`nullptr` to free it early, or let scope exit free it.

#### Parameter ownership

A `unique`-typed parameter is exactly a synthesized `move` parameter - the two
spellings are defined to be equivalent, so they cannot drift apart. The callee's
signature is what declares the ownership sink; the call site is unchanged (no
call-site `move`), and the caller's variable is nulled after the call:

```c
void sink(unique Resource* r) { /* r is owned here; freed on return unless moved out */ }
void borrow(Resource* r)      { /* caller still owns r */ }

unique Resource* c = new Resource();
sink(c);      // transfers ownership; c is nulled after the call

unique Resource* d = new Resource();
borrow(d);    // plain-pointer parameter: this is a BORROW, not a transfer - d still
              // owns it and frees it once at scope exit
```

Passing a `unique` value to a plain (non-`move`, non-`unique`) parameter is always a
borrow - you cannot hand ownership to something that did not ask for it.

#### `unique` interface values

`unique` on an interface-typed local or parameter follows the same owning-location
rule, with one extra restriction: boxing into a `unique` interface only accepts a
**heap** source. Boxing a stack value would require a silent heap-promotion, which the
compiler refuses to do implicitly:

```c
interface IShape { int area(); };
class Circle : IShape { int r = 0; int area() { return r * r; } ~Circle() {} };

unique IShape s = new Circle();   // OK: heap source

Circle t;
unique IShape bad = t;            // error: cannot initialize unique 'bad' from a
                                   // borrowed value - ... (t is a stack value)
```

Deleting a `unique IShape` dispatches the concrete implementor's destructor through the
vtable, then frees the object - exactly like `delete` on a scalar interface value today.

#### `unique` containers

A container's element type is itself a type argument, so `unique` there follows the
same rule as everywhere else: it marks the element position as an owning slot. A
`list<T*>` or `list<IShape>` with a **bare** element type is a **borrowed view** - it
never frees its elements, no matter how they were added. Mark the element type
`unique` to make the list own them:

```c
import "list.cb";

struct Payload { int v = 0; ~Payload() {} };

list<unique Payload*> owner;         // OWNS its elements
owner.add(new Payload());
owner.add(new Payload());
// owner's destructor deletes every element it still holds

list<Payload*> view;                 // BORROWS - never frees
view.add(new Payload());             // caller (or nobody) is still responsible for freeing
```

`list<unique T*>` and `list<T*>` (likewise `list<unique IShape>` and `list<IShape>`)
are **distinct instantiations** with no implicit conversion between them - a function
that takes one does not accept the other.

Container access mirrors the local/parameter rules:

- `[]` / `get(i)` return a **borrow** (`alias`) - reading an element never transfers it out.
- `take(i)` removes an element and transfers ownership to the caller.
- `set(i, move value)` destructs the old owning element (if any) before storing the new one.
- `removeAt(i)` / `clear()` / the destructor free every owning element they discard.
- `.copy()` on a `unique`-element list is a **compile error**: "cannot copy a list of
  unique elements; use move or copy elements explicitly" - a bitwise copy of an owning
  element would hand one pointee to two owners. Bare-pointer lists are unaffected:
  `.copy()` on a borrowed view produces another (correctly) aliasing shallow copy.

> **Breaking change.** Before `unique` existed in this position, a bare `list<T*>` /
> `list<IShape>` owned its pointer/interface elements by default. That is no longer
> true: a bare element type is now always a borrowed view. Code that relied on the old
> owning-by-default behavior must add `unique` to the element type
> (`list<T*>` -> `list<unique T*>`) or it will silently stop freeing its elements.
>
> As of this writing `dictionary<K,V>` and `hashset<T>` have **not** been migrated to
> `is_unique` - their pointer-valued keys/values still key off `is_pointer` and own bare
> pointer elements exactly as before. Only `list<T>` follows the `unique` rule above;
> migrating the other containers is planned but not yet done. Check `is_pointer(V)` vs
> `is_unique(T)` in the container's source if in doubt.

**Pitfall: feeding a borrowed list from a named owning local leaks.** `list<T>.add`
takes `move T`, so adding a *named* pointer local always nulls the source - even when
the target list is a bare (borrowed) list that will never free it:

```c
list<Payload*> view;               // borrowed - never frees
Payload* p = new Payload();
view.add(p);                       // p is nulled; nobody owns the Payload anymore - LEAK
```

Feed a borrowed list from an rvalue borrow instead (e.g. `owner[i]`, which is already a
borrow and is not nulled by `add`):

```c
list<unique Payload*> owner;
owner.add(new Payload());

list<Payload*> view;
view.add(owner[0]);                // OK: owner[0] is a borrow; owner still owns it
```

#### `is_unique(T)` and `compile_error`

`is_unique(T)` is a compile-time intrinsic, the `unique` counterpart to `is_pointer(T)`:
inside a generic instantiation it returns 1 if `T` resolves to a `unique`-qualified
pointer or interface type, 0 otherwise. Like the other `is_*` intrinsics it is only
meaningful inside `if const`:

```c
if const (is_unique(T))
{
    // T is `unique X*` or `unique IShape` in this instantiation - this branch owns
}
```

`compile_error("message")` is a builtin usable inside `if const` in generic code. It
marks the enclosing method as poisoned for that instantiation; the error only fires if
the method is actually called (not merely instantiated), which is what lets
`list<unique T*>.copy()` exist in source without breaking every unique-list
instantiation that never calls `.copy()`.

### Memory Deallocation (`delete`)

Four delete forms cover the different deallocation scenarios:

| Form | When to use |
|------|-------------|
| `delete ptr` | Free a single object allocated with `new T` or `new T(...)`. Calls the destructor if one is defined. |
| `delete[] ptr` | Free a primitive array allocated with `new T[n]` where `T` has no destructor. Does NOT call destructors. |
| `delete[n] ptr` | Free a struct/class array of `n` elements. Calls the destructor on each element in order, then frees the buffer. |
| `delete[_] ptr` | Free a raw buffer without calling any destructors. Use this for arrays of primitives when `delete[]` would be ambiguous, or to free just the backing storage of a container. |

```c
// Single object
MyStruct* s = new MyStruct;
delete s;                     // calls ~MyStruct() if defined

// Primitive arrays
i8* buf = new i8[1024];
delete[] buf;                 // or: delete[_] buf  (equivalent for primitives)

// Struct arrays - must supply the count
Widget* ws = new Widget[n];
delete[n] ws;                 // calls ~Widget() n times, then frees

// Raw buffer of a struct array - no destructors
Widget* raw = new Widget[n];
delete[_] raw;                // frees memory only, no destructor calls
```

`delete[]` is rejected for struct types - the compiler requires you to choose
explicitly between `delete[n]` (with destructors) and `delete[_]` (without).
`delete[0]` is also rejected; use `delete[_]` to free a zero-or-unknown-count buffer.

> **Owning containers copy = double-free.** `list<T>`, `dictionary<K,V>`, and similar
> containers own their backing buffers. Assigning one by value (e.g. field assignment)
> shallow-copies the header but shares the buffer. Both copies then free the same
> memory at scope exit - heap corruption. Pass containers by pointer or use `move` to
> transfer ownership.
>
> ```c
> list<int> a; a.add(1);
> list<int> b = a;   // WRONG: b and a share the backing buffer -> double-free
> ```
>
> `string` is different: `list<string>::add` and `dictionary::set` take `move string`
> and deep-copy a borrowed (non-owning) argument, so strings stored in containers are
> safe. A plain `string s2 = s1;` assignment also shares the buffer - own an
> independent copy with `s1.copy()`.
>
> For `list<unique T*>` / `list<unique IShape>` specifically, `.copy()` is not merely
> unsafe - it is a **compile error** ("cannot copy a list of unique elements; use move
> or copy elements explicitly"), because a bitwise copy of a `unique` element would
> hand one pointee to two owners. Bare (borrowed) `list<T*>` still allows `.copy()`,
> and it is correct there: two lists of non-owning references aliasing the same objects
> is exactly what "borrowed" means. See [`unique` Ownership](#unique-ownership).

### Discarding a Result (`_`)

`_` is the discard target. `_ = expr;` evaluates the right-hand side for its side effects and
throws the value away. It works for any result type, and if the value is an owning temporary
(an owning struct, `string`, or closure) it is destructed at the end of the full expression -
so discarding never leaks:

```c
_ = compute();        // ignore a return value
_ = makeToken();      // owning struct: built, then freed at the end of this statement
```

A bare call statement behaves the same way - `makeToken();` on its own also frees its result -
so `_ =` is mostly about signalling intent ("I am deliberately ignoring this").

`_` is reserved: it cannot name a variable, and only the plain `=` form is allowed (`_ += x` is
an error). It is also the wildcard in tuple destructuring (see
[Destructuring Declaration](#destructuring-declaration)) and in `switch` arms (`case _ =>`).

---


## Namespaces & Modules

### Namespaces

```c
namespace MathUtils
{
    int add(int a, int b)      { return a + b; }
    int multiply(int a, int b) { return a * b; }

    namespace Advanced
    {
        int square(int x) { return x * x; }
        int cube(int x)   { return x * x * x; }
    }
}

MathUtils.add(3, 4);             // 7
MathUtils.Advanced.square(4);    // 16
```

A dotted name declares the nested chain in one line - `namespace os.windows { ... }`
is equivalent to `namespace os { namespace windows { ... } }`.

Namespaces can also hold `extern` declarations. The function registers under its
qualified lookup name but keeps its bare linkage symbol, so the namespace isolates
the *name*, not the import: core's `os.windows.Sleep` and a header-imported bare
`Sleep` bind to the same Win32 function.

```c
namespace os.windows
{
    extern stdcall void Sleep(i32 ms);   // links against "Sleep"
}

os.windows.Sleep(10);   // qualified lookup, bare linkage
```

Namespaces can also hold global variables (including `const`). They are scoped to
the namespace: access them as `Namespace.name` from outside, or by the bare name
from inside the namespace (a nested namespace sees its parents' globals). The bare
name is not visible outside the namespace, and two namespaces can each declare a
global of the same name without colliding.

```c
namespace Cfg
{
    int counter = 0;
    const int WIDTH = 120;

    void bump() { counter++; }   // bare sibling access inside
}

int w = Cfg.WIDTH;   // 120 - qualified access from outside
Cfg.counter = 5;     // qualified write
int c = counter;     // error: 'counter' is not visible outside Cfg
int x = Cfg.HEIGHT;  // error: 'HEIGHT' is not a member of namespace 'Cfg'
```

### `using` Aliases

```c
using Math    = MathUtils;           // global alias
using MathAdv = MathUtils.Advanced;  // alias to nested namespace

Math.add(3, 4);    // 7
MathAdv.square(5); // 25

void myFunc()
{
    using M = MathUtils;   // local alias, only visible inside this function
    M.multiply(2, 3);      // 6
}
```

An alias may also name a type form, not just a namespace - a generic instantiation, a pointer,
or a fixed array:

```c
using IntList = list<int>;   // generic instantiation - IntList xs; xs.add(1);
using Handle  = void*;       // pointer alias - lowers to i8*
using Vec3    = float[3];    // fixed-array alias - Vec3 v = default; v[0] = 1.0f;
```

It may also name a **function (callback) type** - either a thin `function<R(Args)>` or a fat
`Lambda<R(Args)>` (see [Function Pointers](#function-pointers)). The alias is usable everywhere the
spelled-out type is: a parameter, a local, a struct field, or a return type. For a `Lambda<...>`
alias, ownership of any captured state flows through it unchanged.

```c
using WaitCallback  = function<void(void*, bool)>;  // thin: a WAITORTIMERCALLBACK (C ABI) shape
using EventCallback = Lambda<void(void*)>;            // fat: an event/handler that may capture
using IntUnaryOp    = Lambda<int(int)>;               // fat: makeAdder below captures `base`

struct Button { EventCallback onClick = default; }  // alias as a field type

int apply(IntUnaryOp f, int x) { return f(x); }     // alias as a parameter type
IntUnaryOp makeAdder(int base)                      // alias as a return type
{
    IntUnaryOp f = (int x) => x + base;             // captures base -> must be a Lambda<...>
    return f;
}
```

The `*` / `[N]` suffix is part of the alias and is folded onto the use site: `using PP = int*;`
then `PP* x;` yields `int**` (two pointer levels are the maximum; three is an error). A pointer
to an aliased fixed array (`Vec3* p`) is rejected like a written `float[3]*`.

A structured RHS (generic, pointer, or array) must resolve to a known type or the compiler
errors. A bare-identifier RHS that does not name a known type is treated as a namespace alias
(as in the first example) - so a namespace can be aliased before it is defined.

### Module Imports

Split code across multiple files with `import`:

```c
import "list.cb";
import "dictionary.cb";
```

Imported functions, structs, and namespaces are available immediately after the import.

#### Grouped imports (`{ ... }`)

A brace-wrapped comma list imports several files on one line - a shorthand for writing the
same plain `import "x";` statements separately:

```c
import { "list.cb", "dictionary.cb" };
import { "sqlite3.h", "sqlite3.c" };   // binds the header, compiles the .c (see C Interop)
```

A `.cb` or `.c` entry routes exactly like a bare `import "x";` (the dispatch is per entry), so a
group can mix CFlat sources and C inputs. A trailing comma is allowed.

The **C-header entries of a group share one translation unit**: the extraction stub `#include`s
each header in listed order, so an earlier entry satisfies a later one's prerequisites. This is
how you bind a header that is not self-contained on its own - e.g. `tlhelp32.h` needs
`windows.h` included first:

```c
import { "windows.h", "tlhelp32.h" } lib { "user32.lib", "gdi32.lib" };
```

cflat has no built-in knowledge of any specific header; the dependency lives entirely in your
source. (A bare `import "tlhelp32.h";` fails with a diagnostic suggesting this grouped form.)
Each header still registers only the decls in its own directory, so the group does not
over-expose. See [C Interop](C_INTEROP.md) for details.

Group-level `lib`, `define`, and `cache` clauses apply to the **whole group**:

```c
import { "windows.h", "shlwapi.h" } cache;   // both headers opt into the disk cache
```

(`cache` is a no-op for `.cb` / `.c` entries - only the `.h` header-bind path consults it; the
group's header entries are folded into the cache key together.) An `as` alias is meaningful only
when the group holds a single filename, since one alias cannot name several files; use separate
`import` lines when you need an alias or a per-entry `lib` / `define`.

#### Import alias (`as`)

Give an imported file a local alias so its symbols are accessible under a qualified name:

```c
import "ns_math.cb" as Math;

int s = Math.square(5);   // calls square() from ns_math.cb
Math.Point p;             // uses Point struct from ns_math.cb
```

`import "file.cb" as Alias` is a caller-side alias, not a namespace boundary. The file's symbols are compiled as-is (unqualified names in the file continue to work), and `Alias.X` resolves to `X` only if `X` was declared in that file. Accessing a name not contributed by the file is a compile error.

If two imported files both declare a symbol with the same name and signature, that is a collision - the author should put the conflicting declarations inside a `namespace` block to make them distinct.

#### macOS frameworks (`import framework`)

On a macOS (Darwin) target, `import framework` links an Apple framework as a Mach-O load
command, so its symbols become plain externs instead of runtime `dlopen`/`dlsym` lookups:

```c
import framework "AppKit";                     // single framework
import framework { "AppKit", "Foundation" };   // several on one line
```

To bind a framework's C headers (auto-extern the declarations) AND link it, add a `framework`
clause to a header/package import:

```c
import package "CoreGraphics/CoreGraphics.h" framework "CoreGraphics";
```

Linking is **SDK-free after a one-time `cflat --init`**: the harvest writes tbd stubs for
AppKit / Foundation / CoreFoundation (plus `libobjc`) under `~/.cflat/macsdk`, and the bundled
`ld64.lld` resolves the frameworks from there - no Xcode or Command Line Tools needed. Header
**binding** (the `framework` clause on a header import) still needs a real SDK (`$SDKROOT` or
`xcrun`) because the harvested stubs carry no headers. Objective-C frameworks such as AppKit are
driven through the objc runtime (see `core/cocoa.cb`), which links AppKit + Foundation this way
and obtains `objc_msgSend` via `dlsym(RTLD_DEFAULT, ...)`. Targeting a non-macOS platform with
`import framework` is a compile error. See [C Interop](C_INTEROP.md) and the `--framework` flag
in [CLI](CLI.md).

#### Deploying a package resource (`pri` clause on `package-nuget`)

A `pri "<file>"` clause on an `import package-nuget` line locates the named `.pri` inside the
resolved NuGet package and copies it next to the output exe as `<exe>.pri` (WinUI's MRT probes
`resources.pri` and `<exe>.pri` beside an unpackaged exe). It is only valid on `package-nuget`
imports and deploys only when emitting a native exe:

```c
import package-nuget "Microsoft.UI.Xaml.winmd" from "Microsoft.WindowsAppSDK.WinUI/1.8.260224000" pri "Microsoft.UI.Xaml.Controls.pri";
```

---

## Type Casting

C-style casts truncate or sign-extend between numeric types:

```c
float f = 3.9f;
int   i = (int)f;    // 3  (truncate toward zero)

i32 neg32 = -1;
i64 big   = (i64)neg32;   // sign-extended to -1

i64 big64 = 0x200000005;
i32 trunc = (i32)big64;   // 5  (low 32 bits)
```

### `bool` in arithmetic

`bool` is an `i1`. The comparison operators (`==`, `!=`, `<`, `<=`, `>`, `>=`)
and the logical operators (`&&`, `||`, `!`) yield a normalized `bool` - exactly
`0` (false) or `1` (true), never a wider unspecified value. A `bool` widens to an
integer as that same `0`/`1` (a `zext i1`), both implicitly in arithmetic and via
an explicit `(int)` cast - the two forms emit identical IR, so the cast is
optional:

```c
int j = 0;
j = j + (a >= b);        // +1 when a >= b, +0 otherwise (no cast needed)
j = j + (int)(a >= b);   // identical: redundant cast

bool flag = (a >= b);
int  x = flag;           // x is 0 or 1
```

Because the result is guaranteed `0`/`1`, summing comparisons is well-defined -
the basis for branchless code such as counting how many sorted keys are `<= key`:
`j += (key >= keys[k])` over the run gives the exact rank with no data-dependent
branch.

UFCS integer narrowing methods - each wraps to the target width:

```c
i32 v = 128;
i8  b = v.to_i8();    // -128 (wraps)
v.to_i16();           // 128
v.to_i32();           // 128
v.to_i64();           // 128
```

Float UFCS methods:

```c
(3.7f).floor();    // 3.0
(-3.7f).ceil();    // -3.0
(3.5f).round();    // 4.0
(-5.5f).abs();     // 5.5
(9.0f).sqrt();     // 3.0
```

### Integer <-> pointer casts

A C-style cast moves freely between any integer type, any pointer type, and across
the two. This is essential for C interop, where the Win32 `LPARAM` / `WPARAM` /
`HANDLE` / `HMENU` types are passed as integers that actually carry pointers or
opaque handles:

```c
i64 lParam = ...;
Node* p   = (Node*)lParam;     // integer -> pointer
i64   back = (i64)p;           // pointer -> integer
void* h    = (void*)(i64)id;   // small integer widened, then -> pointer
Foo*  f    = (Bar*)b;          // pointer -> pointer (reinterpret, no runtime cost)
```

| Conversion | Allowed | Behavior |
|------------|---------|----------|
| integer -> pointer | explicit cast | the integer bits become the address; a narrower integer is zero-extended to pointer width |
| pointer -> integer | explicit cast | the address becomes an integer; **truncated** if the target integer is narrower than a pointer |
| pointer -> pointer | explicit cast | pure reinterpret, zero runtime cost |
| literal `0` -> pointer | **implicit** | yields a null pointer (this is what `= default` / null-init does) |

Only the integer literal `0` converts to a pointer implicitly (the null case).
Every other integer<->pointer move requires the explicit cast - there is no
implicit conversion of a general integer to a pointer or back.

**Round-trip handles and addresses through a 64-bit integer, never `i32`.** On the
x64 target a pointer is 64 bits, so `(i32)somePointer` silently drops the top half
of a real handle. Use `i64` / `u64`:

```c
HANDLE h = ...;
i64 token = (i64)h;            // safe round-trip
HANDLE h2 = (HANDLE)token;     // recovers the original handle

i32 bad = (i32)h;              // WRONG: truncates a 64-bit handle to 32 bits
```

Likewise, when widening a small integer (an enum value, a control id) into a
pointer slot, cast it to `i64` first - `(void*)(i64)id` - so the value occupies the
full pointer width rather than relying on the narrow integer's extension.

A struct **value** cast to a pointer is rejected (use `&value` or `getPtr()`); only
integers and other pointers reinterpret to a pointer.

### Unsigned narrow-integer casts: always widen to `int` before use

A cast to an unsigned narrow type like `u8` produces a signed `i8` in LLVM IR.
When you then use the result as an array index or in a comparison, the signed
interpretation means values >= 128 appear negative and index or compare incorrectly.

Always widen to `int` (or another signed integer) after an unsigned narrow cast
before using the value as an index or operand:

```c
i8* data = ...;
int[256] freq;

// WRONG: (u8)data[i] is still i8 in IR; values >= 128 sign-extend to negative
freq[(u8)data[i]]++;         // mis-indexes for bytes >= 128

// CORRECT: widen to int first so the zero-extension is explicit
freq[(int)(u8)data[i]]++;    // correct zero-extension to 0..255
```

The same applies to comparisons and any arithmetic that must treat the byte as
unsigned: `if ((int)(u8)c >= 128)`, `int idx = (int)(u8)src[i];`.

---

## Null-Safety

### Null-Conditional Access (`?.`)

Dereferences a pointer only if it is non-null; short-circuits to zero/null otherwise:

```c
struct Node { int value = 99; };

int readValue(Node* node)
{
    return node?.value;   // 0 if node is null
}
```

### Null-Coalescing (`??`)

Provides a fallback when the left side is null (pointers) or zero (integers):

```c
int read(Node* node)
{
    return node?.value ?? -1;   // -1 if node is null
}

int x = 0;
int y = x ?? 42;   // 42  (x is zero, treated as "empty")

const char* name = tryGetName() ?? "Unknown";
```

### `nullptr`

`nullptr` is the null pointer constant - a pointer that points at nothing. Use it
to initialize, clear, or compare any pointer type. CFlat spells it `nullptr` (not
`null` or `NULL`):

```c
Node* p = nullptr;          // explicit null initializer
p = nullptr;                // clear a pointer

if (p == nullptr) { ... }   // null check
if (p != nullptr) { ... }   // non-null check
```

- A pointer field with no declared initializer is already zero-filled to `nullptr`
  by `= default`, so an explicit `= nullptr` is only needed when you want it stated.
- The integer literal `0` also converts to a null pointer implicitly, but prefer
  `nullptr` for clarity - it documents intent and reads as a pointer, not a number.
- Pass `nullptr` for optional C-interop arguments that expect a pointer
  (`GetModuleHandleA(nullptr)`, a `nullptr` window handle, etc.).
- `nullptr` carries no ownership, so assigning it never triggers a move and is the
  way to break a `view`/`span` bond (see [Ownership](#ownership--lifetime-move-keyword)).

Dereferencing or member-accessing a `nullptr` is undefined at runtime; reach for
the null-safe `?.` operator above when a pointer may legitimately be null.

---

## Type Introspection

### `typeof`

Returns the type name of an expression or type as a `const char*` at compile time:

```c
int  i = 0;
bool b = false;

typeof(i)         // "int"
typeof(b)         // "bool"
typeof(int)       // "int"
typeof(MyStruct)  // "MyStruct"
```

### `nameof`

Returns the name of a variable, field, or type as a `const char*` at compile time:

```c
int myVar = 0;
nameof(myVar)        // "myVar"
nameof(thing.value)  // "value"
nameof(MyStruct)     // "MyStruct"
```

### `sizeof` / `alignof`

```c
sizeof(int)        // size in bytes
alignof(MyStruct)  // alignment in bytes
```

A variable name also works: the operand measures the variable's declared
storage. For a fixed-size array that is the full byte capacity, which is the
common buffer-capacity idiom in C interop:

```c
char[128] buf;
u32 cap = (u32)sizeof(buf);   // 128
int x = 5;
sizeof(x)                     // 4 - same as sizeof(int)
```

On a name collision the type meaning wins, matching C. The result is typed
`i64`.

---

## Annotations

Annotations are user-defined metadata attached to struct fields, classes, or interfaces. Define an annotation struct with `annotation`, then apply it with `[Name]` or `[Name(args)]`. An annotation must be declared (in the current file or an import) before use; an undeclared annotation is an "Unknown annotation" error. The COM/WinRT `[winrt]` and `[uuid(...)]` markers are ordinary annotations declared in `com.cb`.

```c
annotation Private   { };
annotation JsonName  { string value; };
annotation MaxLength { int value; };

struct Person
{
    [JsonName("full_name")]
    string name = default;

    [Private]
    string _token = default;

    [JsonName("age_years")]
    [MaxLength(50)]
    int age = 0;
};
```

Read annotation values at compile time with `annotationof(StructType, "fieldName", "AnnotationName")`. Returns a `string` - `"1"` for marker annotations, the serialized value for single-field annotations, or empty if the annotation is absent:

```c
string fn  = annotationof(Person, "name",   "JsonName");   // "full_name"
string prv = annotationof(Person, "_token", "Private");    // "1"
string ml  = annotationof(Person, "age",    "MaxLength");  // "50"
string none = annotationof(Person, "age",   "Private");    // ""  (not present)
```

An annotation applied to a class or interface is read with the two-argument form `annotationof(Type, "AnnotationName")`:

```c
[winrt] class Cat : IPurr { ... };

string isWinrt = annotationof(Cat, "winrt");   // "1"  (present)
string none    = annotationof(Cat, "Private"); // ""   (not present)
```

---

## Reflection

Reflection walks a struct's fields at runtime and dispatches to visitor callbacks. Import `interfaces.cb` to get `IReflector`.

### `IReflector` interface

```c
interface IReflector
{
    void visitInt(string name, int value);
    void visitBool(string name, bool value);
    void visitFloat(string name, float value);
    void visitString(string name, string value);
    void visitNull(string name);
    void beginObject(string name);
    void endObject();
    void beginArray(string name);
    void endArray();
};
```

### `reflect(value, visitor)`

`reflect` is a compiler intrinsic that iterates every field of a struct and dispatches to the appropriate visitor method. Nested structs recurse with `beginObject`/`endObject`. `list<T>` fields call `beginArray`/`endArray`. Fields marked `[Private]` are skipped:

```c
struct Point { int x = 0; int y = 0; };

// Implement IReflector
class Printer : IReflector
{
    void visitInt(string name, int v)    { printf("%s=%d ", name.data(), v); }
    void visitBool(string name, bool v)  { }
    void visitFloat(string name, float v){ }
    void visitString(string name, string v){ }
    void visitNull(string name)          { }
    void beginObject(string name)        { printf("%s:{", name.data()); }
    void endObject()                     { printf("} "); }
    void beginArray(string name)         { }
    void endArray()                      { }
};

Point p;
p.x = 3;
p.y = 7;

Printer pr;
IReflector visitor = pr;
reflect(p, visitor);    // prints: x=3 y=7
```

`reflect` also works on null pointers - calls `visitNull` with the field name.

---

## Range-Based For

Iterate over any type that provides `count()` and `get(int)` methods (including `list<T>`, `array<T>`), or directly over C-style fixed arrays:

```c
import "list.cb";

list<int> nums;
nums.add(10);
nums.add(20);
nums.add(30);

for (int x in nums)
    printf("%d\n", x);
```

C-style fixed arrays work directly - no helper methods needed. Use the type-first
`T[N]` syntax (the C-style `T name[N]` declarator is not accepted for fixed arrays):

```c
int[4] buf;
buf[0] = 10; buf[1] = 20; buf[2] = 30; buf[3] = 40;

for (int x in buf)
    printf("%d\n", x);
```

`array<T>` (heap-allocated, fixed-size) also works:

```c
import "array.cb";

array<int> arr;
arr.init(3);
arr.set(0, 100); arr.set(1, 200); arr.set(2, 300);

for (int v in arr)
    printf("%d\n", v);
```

---

## Control Flow

### Ternary Operator (`? :`)

`condition ? trueValue : falseValue` - a compact conditional expression that evaluates to one of two values:

```c
int x = 10;
int abs_x = (x >= 0) ? x : -x;    // abs_x = 10

bool flag = true;
string label = flag ? "yes" : "no"; // label = "yes"
```

Both branches must produce compatible types. The ternary is an expression, so it can appear anywhere a value is expected:

```c
printf("sign: %s\n", (n > 0) ? "pos" : (n < 0) ? "neg" : "zero");
```

### `break` and `continue`

`break` exits the nearest enclosing `while`, `for`, or `foreach` loop immediately. `continue` jumps to the loop's next iteration:

```c
// break - exit when sentinel found
int i = 0;
while (i < 100)
{
    if (i == 5) break;
    i++;
}
// i == 5

// continue - skip even numbers
for (int k = 0; k < 10; k++)
{
    if (k % 2 == 0) continue;
    printf("%d\n", k);   // prints 1 3 5 7 9
}
```

Both also work inside `foreach` and `do...while` loops.

### Pointer Arithmetic

CFlat follows C semantics: arithmetic on a typed pointer steps by **element size**, not by bytes.

```c
int* arr = new int[4];
arr[0] = 10; arr[1] = 20; arr[2] = 30; arr[3] = 40;

// ptr + int / ptr - int
int* p = arr + 1;      // points to arr[1]  (advance 4 bytes)
int* q = arr + 3;
int* r = q - 1;        // points to arr[2]

// Subscript on raw pointer
printf("%d\n", p[0]);  // 20
printf("%d\n", p[1]);  // 30

// p++ / p-- advance by sizeof(*p) elements
p = arr;
p++;                   // now points to arr[1]
p++;                   // now points to arr[2]
p--;                   // back to arr[1]

// p += n / p -= n
p = arr;
p += 3;                // points to arr[3]
p -= 2;                // points to arr[1]

// ptr - ptr -> element count (C ptrdiff_t)
int* start = arr;
int* end   = arr + 4;
int diff = (int)(end - start);   // 4  (not 16 bytes)
```

`i8*` (byte pointer) obeys the same rules - since `sizeof(i8) == 1`, `p++` and `p += n` advance by exactly one byte, as expected:

```c
i8* buf = (i8*)new i8[8];
i8* cursor = buf;
cursor++;              // advance 1 byte
cursor += 3;           // advance 3 more bytes
```

---

## Other Features

### Operator Overloading

Define `operator+`, `operator-`, `operator*`, `operator/`, `operator==`, `operator!=`, `operator<`, `operator>`, `operator<<`, `operator>>`, `operator[]`, `operator new`, `operator delete`, and `operator string` on structs:

```c
struct Vec2
{
    float x = 0;
    float y = 0;

    Vec2 operator+(Vec2 other) { Vec2 r; r.x = x + other.x; r.y = y + other.y; return r; }
    Vec2 operator*(float s)    { Vec2 r; r.x = x * s;       r.y = y * s;       return r; }
    bool operator==(Vec2 other) { return x == other.x && y == other.y; }
};

Vec2 a; a.x = 1; a.y = 2;
Vec2 b; b.x = 3; b.y = 4;
Vec2 c = a + b;          // (4, 6)
Vec2 d = a * 3.0f;       // (3, 6)
bool eq = (a == a);      // true
```

`operator<<` and `operator>>` are overloadable too. The core `channel<T>` uses `operator>>` as a pipe - `src >> dst` forwards every value from one channel into another (see [Threading](THREADING.md)).

**Operator overloading binding rules:**

- **Left-operand dispatch.** `a op b` always calls `a.operator_op(b)`. The struct
  that owns the operator must be the *left* operand. Writing `3.0f * vec` fails if
  only `vec.operator*(float)` is defined; write `vec * 3.0f` instead.

- **Unary operators** are zero-parameter member functions named `operator-`,
  `operator!`, or `operator~`. Apply them with the standard prefix notation:
  `-v`, `!b`, `~bits`.

- **Operator chains on named variables work** (`a + b + c` is fine - each
  intermediate result is a temporary that carries the type and dispatches again).

- **Method calls on unnamed expression results are not supported.** Calling a
  method directly on a parenthesized expression or a string literal produces an
  "unknown function" error. Store the intermediate result first:

  ```c
  // BROKEN
  Vec2 n = (hitPt - center).normalize();  // "unknown function '(hitPt-center)'"
  string u = "hello".toUpper();           // "unknown function '\"hello\"'"

  // Correct: bind the intermediate to a name first
  Vec2 diff = hitPt - center;
  Vec2 n = diff.normalize();

  string s = "hello";
  string u = s.toUpper();
  ```

### Bitwise and Shift Operators

```c
int flags = 0b1010 & 0b1100;   // 0b1000 (AND)
int mask  = 0b1010 | 0b0101;   // 0b1111 (OR)
int xor   = 0b1010 ^ 0b1100;   // 0b0110 (XOR)
int left  = 1 << 4;             // 16
int right = 256 >> 3;           // 32
```

### Numeric Literals

```c
int    dec = 123;
int    hex = 0xFF;
int    oct = 077;
int    uns = 123u;
long   lng = 123L;
float  f   = 1.5f;
double d   = 1e3;    // 1000.0
```

**Default literal width.** A suffix-less literal is typed as the *smallest* type that
holds its value exactly - the literal carries no width of its own, so context (the
target type, an overload, an enclosing expression) can still widen it:

- **Integers**: `5` is `i8`, `300` is `i16`, `100000` is `i32`, and a value past `i32`
  is `i64`. Widening to a larger type is implicit; narrowing is not.
- **Floating point**: a no-suffix decimal is `float` (f32) when it round-trips through
  float without loss of precision, otherwise `double` (f64). So `1.5` and `100.0` are
  `float`, while `0.1` and `3.141592653589793` (which lose precision in float) are
  `double`. The `f` suffix always forces `float`.

```c
auto a = 1.5;    // float  - exact in f32
auto b = 0.1;    // double - not exact in f32
auto c = 1.5f;   // float  - explicit suffix
```

> Because narrowing is never implicit, a literal that the compiler typed as the wider
> form does not silently fit a narrower target: assigning `0.1` (double) to a `float`
> needs an explicit cast, just as assigning `300` (i16) to an `i8` does.

### Built-in Identifiers

```c
printf("file: %s\n",     __FILE__);
printf("function: %s\n", __FUNCTION__);
printf("line: %d\n",     __LINE__);
printf("platform: %d\n", __PLATFORM__);  // 64 or 32, set by -p flag
```

### Reserved Keywords and Intrinsics

The following identifiers are reserved by the compiler and cannot be used as
variable, function, struct, or namespace names.

**Hard keywords** (ANTLR lexer tokens - always reserved):

`annotation`, `as`, `auto`, `bool`, `break`, `case`, `cdecl`, `char`, `class`,
`const`, `continue`, `declspec`, `default`, `delete`, `do`, `double`, `else`,
`enum`, `extern`, `float`, `for`, `function`, `goto`, `if`, `in`, `inline`, `int`,
`interface`, `is`, `long`, `move`, `namespace`, `nameof`, `new`, `operator`,
`register`, `restrict`, `return`, `short`, `signed`, `sizeof`, `static`,
`stdcall`, `string`, `struct`, `switch`, `typedef`, `typeof`, `union`,
`unsigned`, `using`, `void`, `volatile`, `where`, `while`

`stdcall`, `cdecl`, and `declspec` are function specifiers (calling-convention /
`__declspec(...)` markers on an `extern` declaration); `inline` is the same family.

**Soft keywords** (text-matched by the listener - reserved in the positions where
they have syntactic meaning, but legal as identifiers in other positions):

`bond`, `cache`, `define`, `from`, `lib`, `lock`, `program`, `vectorize`

Note: `program` as a top-level keyword defines the managed-entry-point construct
(see [`program` Keyword](#program-keyword)) and cannot be used as a variable name
in the enclosing scope. `in` is a hard keyword and cannot be used as a variable
name at all.

**Reserved compiler intrinsics** (built-in pseudo-functions - cannot be redefined):

`annotationof`, `expect_error`, `is_pointer`, `nameof`, `reflect`, `reflect_set`,
`sizeof`, `typeof`

**Compiler-recognized methods** (not reserved - you define them on a type, the
compiler auto-invokes them by name):

`copy` - the copy constructor for an owning value type (`string`, `wstring`,
containers, closures): `b = a.copy()` makes an independent deep copy instead of the
default move, and the compiler synthesizes `copy` calls when it must duplicate an
owned field or element rather than alias it. See
[Ownership / Lifetime](#ownership--lifetime-move-keyword).

If you accidentally use a reserved word as an identifier, the error message may
point to a nearby token rather than the reserved word itself. Check your names
against this list when the error location looks wrong.

### Compile-Time Conditionals (`if const`)

The `if const` statement evaluates its condition at compile time, eliminating dead branches from the generated code. Works at both function scope and file scope:

```c
if const (__PLATFORM__ == 64)
{
    // Only compiled on 64-bit platforms
    i64 bigNum = 0x123456789ABCDEF0;
}
else
{
    // Only compiled on 32-bit platforms
    i32 num = 0x12345678;
}
```

The untaken branch is discarded before type checking - it is never name-resolved
or code-generated. That is what lets each arm call APIs that only exist on its own
platform, and lets two arms define the same function without colliding.

**Chaining.** `else if const` selects the first arm whose condition is true, at both
scopes. The trailing `else` is optional:

```c
if const (__WINDOWS__)
{
    int platformId() { return 1; }
}
else if const (__MACOS__)
{
    int platformId() { return 2; }
}
else
{
    int platformId() { return 3; }
}
```

**What an arm may contain** depends on the scope. At file scope each arm holds
declarations - functions, structs, globals, `import`, `using`, and nested `if const`.
At function scope each arm holds ordinary statements.

---

## `program` Keyword

`program` defines a struct-like construct with a managed entry point. The compiler auto-generates a `run(list<string>)` method that spawns a dedicated thread, installs a per-thread `BlockAllocator` (all heap allocations inside `main` are freed automatically on return), calls `main`, and joins the thread.

```c
import "list.cb";
import "thread.cb";

program MyApp
{
    int startFrom = 0;   // configurable field

    int main(move list<string> args)
    {
        return startFrom + args.count();
    }
};

extern int main()
{
    MyApp app;
    app.startFrom = 10;

    list<string> args;
    args.add("" + "a");
    args.add("" + "b");

    app.run(args);
    app.WaitForExit();
    return app.exitCode;   // 12
}
```

**Synthetic fields on `program`**:

| Field | Type | Description |
|-------|------|-------------|
| `exitCode` | `int` | Exit code returned by `main`; valid after `WaitForExit()` |
| `onStdout` | `Lambda<void(string)>` | Called for each line printed to stdout inside `main` (may capture state) |
| `onStderr` | `Lambda<void(string)>` | Called for each line printed to stderr inside `main` (may capture state) |
| `inbox` / `outbox` | `arena_channel<IMessage>*` | Zero-copy messaging mailbox; wire with `producer >> consumer`, then send via `outbox` and receive via `inbox` inside `main` (requires `import "arena_channel.cb"` and `useChannel = true` on both) |
| `useChannel` | `int` (0/1) | Opt-in flag: `producer >> consumer` only wires the arena mailbox when **both** programs set `useChannel = true` before the `>>`. Defaults to 0, so a pipe used only for stdout pays nothing for an arena channel. |

**Piping two programs**: `producer >> consumer` wires up to two paths in one step:
- **stdout -> stdin stream (always)**: the compiler synthesizes a `stream`, connects the producer's stdout to the consumer's stdin, and auto-closes it after the producer's `main` returns. So `printf` in the producer is read by `fgets` in the consumer - no hand-written `stream`/`close()`. (Equivalent to the explicit `producer >> s; s >> consumer;` form, which you use when you want to configure `bufferSize` or close manually.)
- **arena mailbox (opt-in)**: when **both** programs set `useChannel = true` before `>>`, the consumer's `inbox` is bound to the producer's `outbox` for structured `IMessage` passing. Without it, no arena channel is allocated.

```cpp
Producer p; Consumer c;
p.useChannel = true;   // both sides must opt in for the mailbox
c.useChannel = true;
p >> c;                // stdout->stdin wired always; arena mailbox wired here
c.run(a2); p.run(a1);  // run the consumer first (it blocks on inbox.recv())
p.WaitForExit(); c.WaitForExit();
```

Run the consumer and producer, then `WaitForExit()` on both; the synthesized stream is cleaned up at scope exit.

**Requirements**: `import "list.cb"` and `import "thread.cb"` must appear before the `program` definition.

---

## C Interop

CFlat can compile your own `.c` source, import another file's `main` as a program, and bind prebuilt C libraries via header extraction or vcpkg. See [`C_INTEROP.md`](C_INTEROP.md) for the full reference (`import "x.c"`, `import program`, `import package`, `import package-vcpkg`, and the `--c-*` / `--vcpkg-*` flags).

---

## WinMD / COM

CFlat can author a COM/WinRT object with `[winrt] class` (it lowers to a thin-pointer COM object with the WinRT HRESULT ABI and `HResult<T>`), consume WinRT metadata with `import "Foo.winmd";`, and emit a `.winmd` with `--emit-winmd`. See [`WINMD.md`](WINMD.md) for the full reference.

---

## JSON (lib/json.cb)

`json.cb` provides `JsonBuilder.toJson<T>` and `JsonBuilder.fromJson<T>`. It respects `[JsonName]` and `[Private]` annotations (requires `interfaces.cb`):

```c
import "json.cb";

annotation JsonName { string value; };
annotation Private  { };

struct Person
{
    [JsonName("full_name")]
    string name = default;
    int    age  = 0;
    [Private]
    string _token = default;
};

Person p;
p.name = "Alice";
p.age  = 30;
p._token = "secret";

string json = JsonBuilder.toJson(p);
// {"full_name":"Alice","age":30}   - _token omitted

Person p2 = JsonBuilder.fromJson<Person>(json);
// p2.name == "Alice", p2.age == 30
```

**Both functions must be called as `JsonBuilder.toJson` / `JsonBuilder.fromJson`.** The
bare unqualified form `fromJson<T>(...)` does not compile.

### Supported field types

| Field type | `toJson` | `fromJson` |
|------------|----------|------------|
| `int`, `bool`, `float` | yes | yes |
| `string` | yes | yes (heap-owned copy) |
| nested struct (value) | yes | yes |
| `list<int>`, `list<bool>`, `list<float>`, `list<string>` | yes | yes |
| `list<struct>` | yes | CURRENT LIMITATION - compile error with misleading message |
| pointer fields | yes (as null) | no |

**String ownership in `fromJson`.** String fields in a deserialized struct are
heap-allocated copies (independent of the JSON input string). They are freed when
the struct goes out of scope. Strings inside a `list<string>` field are also
heap-copied at `add()` time, so the list owns them and they are freed with the list.

**`list<struct>` fields do not round-trip through `fromJson`.** Attempting to
deserialize a struct that contains a `list<NestedStruct>` field produces a compiler
error with a misleading message pointing to a non-existent line. Use `list<int>`,
`list<string>`, or a manually parsed approach for arrays of nested objects.

### Embedding literal braces in strings

`{` and `}` in any string literal trigger interpolation. When you build a JSON
template or embed JSON text inside a string literal, use `{{` and `}}` to produce
literal brace characters:

```c
string template = "{{\"key\": \"{value}\"}}";   // {"key": "<value of 'value'>"}
string literal  = "result: {{value}}";           // "result: {value}"
```

---

## Debug Info

Pass `-g` to emit DWARF debug information for use with debuggers:

```bash
cflat.exe app.cb -o app.ll -g
```

---

## Compiler CLI

Every switch accepted by `cflat.exe`, grouped by purpose, is documented in the
command-line reference: [`doc/CLI.md`](CLI.md).

---

## Standard Library

`core/` is implicitly on the import search path. Only `runtime.cb` is auto-imported; all others require an explicit `import`.

Method-level API discovery is via the LSP (hover / completion / `--symbol`); this
table is a file index of what each library is for.

**Types & collections**

| File | Purpose |
|------|---------|
| `runtime.cb` | Allocator hooks (`new`, `delete`); exit/abort - **auto-imported** |
| `interfaces.cb` | `IString`, `IEnumerable<T>`, `IComparable<T>`, `IReflector`, `ITuple<T...>` |
| `string.cb` | `string` - owned/borrowed UTF-8 value type; `IString` implementation |
| `wstring.cb` | `wstring` - owned UTF-16 string for Win32 `...W` / WinRT APIs |
| `array.cb` | `array<T>` - owning fixed-size heap array with destructor support |
| `list.cb` | `list<T>` - growable array |
| `span.cb` | `span<T>` - non-owning **noalias** window (`T[]` + len); for vectorization. See [HPC.md](HPC.md) |
| `view.cb` | `view<T>` - non-owning **may-alias** window (`T*` + len); sibling of `span<T>` |
| `hashset.cb` | `hashset<T>` - open-addressed set; T must be integer-like |
| `dictionary.cb` | `dictionary<K,V>` - hash map |
| `stack.cb` | `stack<T>` - LIFO |
| `queue.cb` | `queue<T>` - FIFO |
| `pair.cb` | `pair<A,B>` - two-field generic struct |
| `tuple.cb` | `tuple<T...>` - variadic heterogeneous value type |
| `function.cb` | Closure-environment memory primitives backing `function<T>` / lambdas |
| `guid.cb` | `Guid` - 128-bit GUID / RFC 4122 UUID |

**Math & numerics**

| File | Purpose |
|------|---------|
| `math.cb` | `Math` namespace: `abs`/`min`/`max`/`pow`/`sqrt`/`clamp`/`fma`, trig, rounding; constants `Math.PI`/`TAU`/`E`/`SQRT2`/`LN2`/`LN10` |
| `linear_math.cb` | Small fixed-size geometry value types with operator overloading: `vec2`/`vec3`/`vec4` (+ `f` float variants), `mat4`/`mat4f` (row-major, D3D left-handed), `quat`/`quatf`. dot/cross/normalize/lerp/reflect, project/lookAt/inverse, quaternion slerp. GLM-style; for *small* vectors - contrast `hpc/vecmath.cb` |
| `random.cb` | `Random` - splitmix64 PRNG with independent substreams (`jump`/`split`) |
| `intrinsic.cb` | Compiler intrinsics: `popcount`, `ctz`, `clz`, `prefetch`, `likely`/`unlikely`, `pause()`, `cycle_count`/`cycle_count_serialized` |

`cycle_count()` returns the raw hardware tick counter as a `u64` (lowers to `llvm.readcyclecounter`:
RDTSC on x86, `CNTVCT_EL0` on arm64). It is a *tick* count, not a wall-clock value - use it only
for deltas. The tick rate is target-specific and is not a CPU clock in general: x86 RDTSC ticks at
a GHz-scale invariant rate, while arm64's `CNTVCT_EL0` is a fixed ~24MHz timebase that does not
track the core clock. Compare deltas only against other deltas on the same target; never hard-code
a ticks-per-second constant. It is NON-serializing: out-of-order execution may
reorder instructions across the read, so `cycle_count_serialized()` (an LFENCE-bracketed read on
x86) is the choice for tight per-region measurement. Portable and unguarded, unlike time.cb's
x86-only `rdtscp()`/`lfence()`. Prefer it over the QPC-based `Stopwatch`/`Duration` in `time.cb`
when you need sub-100ns resolution and are comparing two counter reads; prefer `time.cb` when you
need a real wall-clock or millisecond duration.

**Memory & allocators**

| File | Purpose |
|------|---------|
| `memory.cb` | `operator new`/`delete` and the active-allocator slot |
| `arena.cb` | `Arena` - bump allocator |
| `arena_allocator.cb` | Mixed-size bump arena backed by the page pool |
| `page_pool.cb` | Process-global pool of 64 KB pages for slab/arena backing |
| `page_arena.cb` | Self-contained bump-allocation region for zero-copy transfer |
| `vmem.cb` | OS virtual-memory abstraction |
| `block_allocator.cb` | Block-based pooled allocator (used by `program` thread startup) |
| `bucket_allocator.cb` | Segregated free-list allocator (size-class buckets) |
| `entry_allocator.cb` | Variable-size bump allocator with per-page entry tables |
| `fit_allocator.cb` | Fit allocation strategy |
| `malloc_allocator.cb` | Thin wrapper around system malloc/free |

**Concurrency**

| File | Purpose |
|------|---------|
| `thread.cb` | `thread<T>` - Win32 thread wrapper |
| `threadpool.cb` | `ThreadPool` - priority work queue with `submit`/`then`/`drain` |
| `mutex.cb` | `mutex`, `atomic<T>`, `lock` statement - synchronization primitives |
| `atomic.cb` | `Atomic<T>` - load/store/fetchAdd over LLVM atomic IR |
| `semaphore.cb` | `Semaphore` - counting semaphore |
| `latch.cb` | `Latch` - one-shot countdown latch |
| `rwlock.cb` | `RwLock` - readers-writer lock |
| `barrier.cb` | Reusable (cyclic) thread barriers |
| `channel.cb` | `channel<T>` - blocking MPMC queue |
| `arena_channel.cb` | Bucket-backed allocator + zero-copy MPMC channel |
| `spsc_queue.cb` | `spsc_queue<T>` - wait-free single-producer/single-consumer ring |
| `stop_token.cb` | `StopSource` / `StopToken` - cooperative cancellation |
| `program.cb` | `program` construct runtime support (thread + allocator lifecycle) |

**I/O, system & time**

| File | Purpose |
|------|---------|
| `filesystem.cb` | `File`, `Path`, `Directory` - file and directory operations |
| `cruntime.cb` | Raw C runtime bindings: `printf` family, stdout/stdin writers, `string.h`/`ctype.h` (hook-aware, VT-correct) |
| `terminal.cb` | Win32 console terminal utilities |
| `stream.cb` | Double-buffered text pipe between two `program` constructs |
| `process.cb` | Launch and communicate with a child process |
| `time.cb` | `TimePoint`, `Stopwatch`, `Timer`, `sleep`, `rdtscp`/`lfence` timing |
| `os.cb` | Platform dispatcher (imports `os.windows.cb` on Windows) |
| `os.windows.cb` | Windows platform externs: Win32 API and MSVC CRT |
| `network/socket.cb` | Platform-agnostic TCP/UDP socket API |
| `network/socket.windows.cb` | Winsock2 raw bindings (pulled in by `socket.cb` on Windows) |

**Serialization & text**

| File | Purpose |
|------|---------|
| `json.cb` | `JsonBuilder.toJson<T>` / `fromJson<T>` - JSON serialization |
| `xml.cb` | `XmlBuilder` - struct-to-XML via the `reflect` intrinsic |
| `regex.cb` | `Regex` - Thompson-NFA (linear-time) regex; capture groups. See [REGEX.md](REGEX.md) |

**COM / WinRT**

| File | Purpose |
|------|---------|
| `com.cb` | Shared COM/WinRT helpers: well-known IIDs and HRESULT codes. See [WINMD.md](WINMD.md) |
| `winrt.cb` | WinRT runtime plumbing: HSTRING bridge, activation, async polling. See [WINMD.md](WINMD.md) |

**Graphics**

| File | Purpose |
|------|---------|
| `graphic/bitmap.cb` | `Bitmap` - load/create/save Windows BMP files; `BitmapPixel` struct |

**HPC** (see [HPC.md](HPC.md))

| File | Purpose |
|------|---------|
| `hpc/parallel.cb` | `parallel_for_n` / `parallel_reduce<T>` (raw-thread and ThreadPool) data parallelism |
| `hpc/vecmath.cb` | BLAS level-1 style dense vector kernels over `double[]` (large dynamic vectors; small fixed `vec3`/`mat4` geometry is in `linear_math.cb`) |
| `hpc/densemat.cb` | Dense row-major matrix kernels (BLAS level-2/3 style) |
| `hpc/factor.cb` | Dense matrix factorization + triangular solve |
| `hpc/sparse.cb` | Compressed sparse row (CSR) matrix and sparse mat-vec |
| `hpc/solvers.cb` | Iterative sparse linear solvers |
| `hpc/stencil.cb` | Structured-grid stencil kernels (finite-difference PDEs) |
| `hpc/scan.cb` | Reductions, prefix sums, and streaming statistics over `double[]` |
| `hpc/fft.cb` | Iterative in-place radix-2 Cooley-Tukey FFT |

**Diagnostics**

| File | Purpose |
|------|---------|
| `diagnostic/heap_audit.cb` | Opt-in heap leak detector (front end). See [CLI.md](CLI.md) `--heap-audit` |
| `diagnostic/thread_fuzz.cb` | Seeded randomized thread-scheduling fuzzer |

---
