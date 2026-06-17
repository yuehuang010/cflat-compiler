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
  - [Compile-Time Conditionals (`if const`)](#compile-time-conditionals-if-const)
- [`program` Keyword](#program-keyword)
- [C Interop](#c-interop) ([full reference](C_INTEROP.md))
- [JSON](#json-libjsoncb)
- [Debug Info](#debug-info-work-in-progress)
- [Compiler CLI](#compiler-cli)
- [Standard Library](#standard-library)
- [Threading & Memory Management](threading.md)
- [HPC & SIMD (`vectorize`, `simd<T,N>`)](HPC.md)

---

## Types

### Primitives

Standard C primitives are all supported: `int`, `char`, `short`, `long`, `float`, `double`, `bool`, `void`.

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

Use `function<ReturnType(ParamTypes...)>` for typed function pointers. The bare `function` keyword infers the type from the assigned function:

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

> **Passing a function to C code.** A CFlat `function<T>` is a 16-byte `{code, env}` closure, not a bare C function pointer. To pass a callback to a C function-pointer parameter (`qsort`, a Win32 `WNDPROC`, ...), pass a **non-capturing named function directly**; a lambda or a `function<>` variable is rejected at compile time because C cannot carry the closure's captured state. Win32 callbacks should also be marked `stdcall`. See [C Interop: passing a CFlat function as a C callback](C_INTEROP.md#passing-a-cflat-function-as-a-c-callback).

### Lambdas

Anonymous functions use C#-style arrow syntax and are compatible with `function<T>`. Capture is automatic - the lambda body is scanned at compile time and outer variables are captured without any explicit capture list.

**Non-capturing lambda** - captures nothing; works identically to a named function:

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

**Value capture** - primitives, pointer types, and `string` are copied into the closure at lambda creation time. Subsequent changes to the outer variable do not affect the captured copy:

```c
int offset = 10;
function<int(int)> addOffset = (int x) => { return x + offset; };
addOffset(5);   // 15
offset = 99;    // does NOT change the captured copy
addOffset(5);   // still 15
```

Pointer captures copy the pointer value - both the lambda and the outer code share the same pointed-to memory:

```c
int* p = new int;
*p = 42;
function<int()> read = () => { return *p; };
*p = 77;
read();   // 77 - both sides access the same heap cell
```

**Reference capture** - non-pointer struct and class values are captured by reference. The lambda holds a pointer to the outer variable; mutations on either side are immediately visible to the other:

```c
struct Counter { int value = 0; };

Counter c;
function<void()> inc = () => { c.value = c.value + 1; };
inc(); inc(); inc();
c.value;   // 3 - lambda mutations are visible in the caller

c.value = 10;   // outer mutation
inc();
c.value;   // 11 - outer mutation was visible inside the lambda
```

**Lifetime constraint** - a lambda that reference-captures a local variable is *bonded* to it. The lambda cannot be returned from the function or assigned to a variable in a wider scope, because doing so would leave the lambda holding a dangling pointer to a stack variable that has gone out of scope:

```c
function<void()> bad()
{
    Counter c;
    function<void()> inc = () => { c.value = c.value + 1; };
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

A tuple value can be unpacked into named local variables in a single declaration. The types must be written explicitly:

```c
(int x, float y) = makePoint();
// x == 3, y == 1.5f

(int a, float b, bool c) = makeTriple();
```

The right-hand side can be any expression that evaluates to a matching tuple type - a variable, a function call, or a construction expression.

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

### Generic Interfaces

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

### `is` and `as` Operators

`is` tests the concrete runtime type of an interface value. `as` downcasts to a concrete struct pointer or interface; returns `nullptr` if the cast fails:

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

Two operations that look natural do not work correctly at this time:

- **Returning an interface value from a function miscompiles.** The generated code
  produces incorrect IR and the return value is wrong at runtime. Workaround: return
  the concrete pointer and let the caller box it into an interface at the assignment site.

  ```c
  // BROKEN - do not return an interface value
  IShape getShape() { Circle c; IShape s = c; return s; }  // wrong output

  // Correct: return the concrete pointer, box at call site
  Circle* getShape() { return new Circle(); }
  IShape s = getShape();   // boxing at assignment works correctly
  ```

- **`interface*` (pointer-to-interface) is rejected.** Interfaces are fat-pointer
  values (object ptr + vtable ptr). You cannot take the address of an interface
  variable. Use a concrete pointer where an indirect reference is needed.

Both of these are known compiler limitations, not intentional language design decisions.

---


## Ownership / Lifetime (`move` keyword)

CFlat uses **context-based ownership**: local variables and struct fields own their pointers; function parameters borrow by default.

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

It may also name a **function (callback) type** - a `function<R(Args)>`. The alias is usable
everywhere the spelled-out type is: a parameter, a local, a struct field, or a return type, and
ownership of any captured state flows through it unchanged.

```c
using EventCallback = function<void(void*)>;        // HANDLE is void*; an event/handler callback
using WaitCallback  = function<void(void*, bool)>;  // e.g. a WAITORTIMERCALLBACK shape
using IntUnaryOp    = function<int(int)>;

struct Button { EventCallback onClick = default; }  // alias as a field type

int apply(IntUnaryOp f, int x) { return f(x); }     // alias as a parameter type
IntUnaryOp makeAdder(int base)                      // alias as a return type
{
    IntUnaryOp f = (int x) => x + base;             // bind to a local, then return it
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
import "utils.cb";
import "math/vector.cb";
```

Imported functions, structs, and namespaces are available immediately after the import.

#### Grouped imports (`{ ... }`)

A brace-wrapped comma list imports several files on one line - a shorthand for writing the
same plain `import "x";` statements separately:

```c
import { "utils.cb", "math/vector.cb" };
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

Annotations are user-defined metadata attached to struct fields. Define an annotation struct with `annotation`, then apply it with `[Name]` or `[Name(args)]`:

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

`operator<<` and `operator>>` are overloadable too. The core `channel<T>` uses `operator>>` as a pipe - `src >> dst` forwards every value from one channel into another (see [Threading](threading.md)).

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

`annotation`, `as`, `auto`, `bool`, `break`, `case`, `char`, `class`, `const`,
`continue`, `default`, `delete`, `do`, `double`, `else`, `enum`, `extern`,
`float`, `for`, `function`, `goto`, `if`, `in`, `inline`, `int`, `interface`,
`is`, `long`, `move`, `namespace`, `nameof`, `new`, `operator`, `register`,
`restrict`, `return`, `short`, `signed`, `sizeof`, `static`, `string`, `struct`,
`switch`, `typedef`, `typeof`, `union`, `unsigned`, `using`, `void`, `volatile`,
`where`, `while`

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
| `onStdout` | `function<void(string)>` | Called for each line printed to stdout inside `main` |
| `onStderr` | `function<void(string)>` | Called for each line printed to stderr inside `main` |
| `inbox` / `outbox` | `arena_channel<IMessage>*` | Zero-copy messaging mailbox; wire with `producer >> consumer`, then send via `outbox` and receive via `inbox` inside `main` (requires `import "arena_channel.cb"` and `useChannel = true` on both) |
| `useChannel` | `int` (0/1) | Opt-in flag: `producer >> consumer` only wires the arena mailbox when **both** programs set `useChannel = true` before the `>>`. Defaults to 0, so a pipe used only for stdout pays nothing for an arena channel. |

**Piping two programs**: `producer >> consumer` wires up to two paths in one step:
- **stdout -> stdin stream (always)**: the compiler synthesizes a `stream`, connects the producer's stdout to the consumer's stdin, and auto-closes it after the producer's `main` returns. So `printf` in the producer is read by `fgets` in the consumer - no hand-written `stream`/`close()`. (Equivalent to the explicit `producer >> s; s >> consumer;` form, which you use when you want to configure `bufferSize` or close manually.)
- **arena mailbox (opt-in)**: when **both** programs set `useChannel = true` before `>>`, the consumer's `inbox` is bound to the producer's `outbox` for structured `IMessage` passing. Without it, no arena channel is allocated.

```cflat
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

If you forget to escape and accidentally write `{"key": "val"}` as a string
literal, the compiler reports an "Undefined variable 'key'" error at the start of
the file rather than at the brace - the `{` triggers interpolation before the
error location can be narrowed.

---

## Debug Info (Work in Progress)

Pass `-g` to emit DWARF debug information for use with debuggers:

```bash
cflat.exe app.cb -o app.ll -g
```

---

## Compiler CLI

```
cflat.exe <input.cb> [options]
```

| Flag | Description |
|------|-------------|
| `-o / --output <file>`  | Output native executable path (`.exe`) |
| `-l / --out-lli <file>` | Output LLVM IR file (`.ll`) |
| `-b / --bitcode <file>` | Output LLVM bitcode file (`.bc`) |
| `-g / --debug-info`     | Emit DWARF debug information |
| `-i / --import-dir <dir>` | Directory to search for imported modules |
| `-p / --platform <target>` | Target platform: `x64` (default) or `x86`; sets `__PLATFORM__` to 64 or 32 |
| `-v / --verbose`        | Print detailed diagnostic messages during compilation |
| `-O1`, `-O2`            | Set optimization level |
| `--c-include <dir>`     | Header search dir for C library bindings (repeatable) |
| `--c-lib <path>`        | Prebuilt C import library (`.lib`) to link (repeatable) |
| `--c-define <NAME[=val]>` | Preprocessor define passed to all clang-cl C compiles/dumps (repeatable) |
| `--vcpkg-exe <path>`    | Explicit `vcpkg.exe` (overrides VS-bundled / `VCPKG_ROOT` / `PATH` discovery) |
| `--vcpkg-manifest <path>` | Explicit `vcpkg.json` (skips the upward walk from the source file) |
| `--vcpkg-triplet <triplet>` | vcpkg triplet (default derived from `--platform`: `x64-windows` / `x86-windows`) |
| `--help`                | Show usage and available options |

See [C Interop](#c-interop) for how the `--c-*` / `--vcpkg-*` flags pair with `import "x.c"`, `import package`, and `import package-vcpkg`.

---

## Standard Library

`core/` is implicitly on the import search path. Only `runtime.cb` is auto-imported; all others require an explicit `import`.

| File | Exports |
|------|---------|
| `runtime.cb` | Allocator hooks (`new`, `delete`); exit/abort - **auto-imported** |
| `interfaces.cb` | `IString`, `IEnumerable<T>`, `IComparable<T>`, `IReflector`, `ITuple<T...>` |
| `tuple.cb` | `tuple<T...>` - variadic heterogeneous value type; fields `item_0`, `item_1`, ...; `size()` |
| `string.cb` | `string` value type, manipulation, `IString` implementation |
| `list.cb` | `list<T>` - growable array; `add(move T)`, `get()`, `set(move T)`, `removeAt()`, `sort(comparator)` |
| `span.cb` | `span<T>` - non-owning **noalias** window (`T[]` + len); pass by value for vectorization. See [HPC.md](HPC.md) |
| `view.cb` | `view<T>` - non-owning **may-alias** window (`T*` + len); `slice(start,end)`; sibling of `span<T>` |
| `hashset.cb` | `hashset<T>` - open-addressed set; T must be integer-like |
| `dictionary.cb` | `dictionary<K,V>` - hash map; `add(K, move V)`, `set(K, move V)`, `get()`, `remove()` |
| `math.cb` | `Math` namespace: `abs`, `min`, `max`, `pow`, `sqrt`, `clamp`, trig, rounding; constants `Math.PI`, `Math.TAU`, `Math.E`, `Math.SQRT2`, `Math.LN2`, `Math.LN10` (namespace `const` globals - no parens) |
| `stack.cb` | `stack<T>` - LIFO; `push()`, `pop()`, `peek()` |
| `queue.cb` | `queue<T>` - FIFO; `enqueue()`, `dequeue()`, `peek()` |
| `pair.cb` | `pair<A,B>` - two-field generic struct |
| `filesystem.cb` | `File.exists()`, `File.readAllText()`, `File.writeAllText()`, `File.move()`, `File.isFile()`, `File.remove()`, `File.copy()`; `Path.combine()`, `Path.getFileName()`, `Path.getDirectory()`, `Path.changeExtension()`; `Directory.list()`, `Directory.listFiles()`, `Directory.createAll()` |
| `thread.cb` | `Thread` - Win32 thread wrapper; `start(fn, ctx)`, `join()` |
| `random.cb` | `Random` - splitmix64 PRNG; `seed(i64)`, `seedFromTime()`, `next()`, `nextInt(min, max)`, `nextDouble()`; independent substreams via `jump(i64)` / `split()` |
| `time.cb` | `Duration`, `TimePoint`, `Stopwatch` - high-resolution timing |
| `mutex.cb` | `Mutex` - Win32 CRITICAL_SECTION wrapper |
| `atomic.cb` | `Atomic<T>` - load/store/fetchAdd with LLVM atomic IR |
| `semaphore.cb` | `Semaphore` - counting semaphore |
| `latch.cb` | `Latch` - countdown latch |
| `rwlock.cb` | `RwLock` - readers-writer lock |
| `channel.cb` | `channel<T>` - blocking MPMC queue |
| `spsc_queue.cb` | `spsc_queue<T>` - wait-free single-producer/single-consumer ring |
| `stop_token.cb` | `StopSource` / `StopToken` - cooperative cancellation |
| `arena.cb` | `Arena` - bump allocator |
| `json.cb` | `JsonBuilder.toJson<T>`, `JsonBuilder.fromJson<T>` - JSON serialization/deserialization |
| `graphic/bitmap.cb` | `Bitmap` - load/create/save Windows BMP files (`load(path)`, `save(path)`, `create(w,h)`, `get(x,y)`, `set(x,y,px)`, `width()`, `height()`); `BitmapPixel` struct (u8 fields `r`, `g`, `b`, `a`). `save` writes a real 24 bpp BMP file to disk. Import as `import "graphic/bitmap.cb";` |

---
