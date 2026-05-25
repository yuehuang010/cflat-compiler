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
- [Functions & Core Collections](#functions--core-collections)
  - [Overloading](#overloading)
  - [Default Parameters](#default-parameters)
  - [Named Parameters](#named-parameters)
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

Define `operator string(T)` to convert arbitrary types to `string`; the core library provides conversions from `char*`, `i32`, and any `IString`.

#### String interpolation

Any string literal containing `{expr}` is automatically treated as an interpolated string - no prefix required. Each `{expr}` segment is converted to `string` via `operator string` and the segments are concatenated at runtime:

```c
string name = "World";
string greeting = "Hello {name}!";   // "Hello World!"

int n = 42;
string msg = "n={n}";               // "n=42"

string a = "foo";
string b = "bar";
string both = "{a} and {b}";        // "foo and bar"
```

Any type with `operator string` (including `IString` implementors) works inside `{}`:

```c
struct Point : IString
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

Fields are named `item_0`, `item_1`, … and can be accessed directly.

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

struct Counter : IReadable
{
    int count = 10;
    int Read() { return count; }
};

// A struct can implement multiple interfaces
struct ScaledValue : IReadable, IScalable
{
    int value = 3;
    int Read()            { return value; }
    int Scale(int factor) { return value * factor; }
};
```

### Interface Parameters (VTable Dispatch)

A function accepting an interface type works with any implementing struct:

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

struct Storage<T> : Container<T>
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

### Module Imports

Split code across multiple files with `import`:

```c
import "utils.cb";
import "math/vector.cb";
```

Imported functions, structs, and namespaces are available immediately after the import.

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

```c
Node* p = nullptr;
```

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

C-style fixed arrays work directly - no helper methods needed:

```c
int buf[4];
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

// ptr - ptr → element count (C ptrdiff_t)
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

### Built-in Identifiers

```c
printf("file: %s\n",     __FILE__);
printf("function: %s\n", __FUNCTION__);
printf("line: %d\n",     __LINE__);
printf("platform: %d\n", __PLATFORM__);  // 64 or 32, set by -p flag
```

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
| `inbox` | `channel<IMessage*>` | Actor-model mailbox; send messages from outside, read inside `main` |

**Requirements**: `import "list.cb"` and `import "thread.cb"` must appear before the `program` definition.

---

## C Interop

CFlat can compile your own `.c` source, import another file's `main` as a program, and bind prebuilt C libraries via header extraction. See [`C_INTEROP.md`](C_INTEROP.md) for the full reference (`import "x.c"`, `import program`, `import package`, and the `--c-*` flags).

---

## JSON (lib/json.cb)

`json.cb` provides `JsonBuilder.toJson<T>` and `fromJson<T>`. It respects `[JsonName]` and `[Private]` annotations (requires `interfaces.cb`):

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

Person p2 = fromJson<Person>(json);
// p2.name == "Alice", p2.age == 30
```

Use `{{` and `}}` to embed literal braces in format strings (they are not interpolated):

```c
string literal = "result: {{value}}";   // "result: {value}"
```

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
| `--help`                | Show usage and available options |

See [C Interop](#c-interop) for how the `--c-*` flags pair with `import "x.c"` and `import package`.

---

## Standard Library

`core/` is implicitly on the import search path. Only `runtime.cb` is auto-imported; all others require an explicit `import`.

| File | Exports |
|------|---------|
| `runtime.cb` | Allocator hooks (`new`, `delete`); exit/abort - **auto-imported** |
| `interfaces.cb` | `IString`, `IEnumerable<T>`, `IComparable<T>`, `IReflector`, `ITuple<T...>` |
| `tuple.cb` | `tuple<T...>` - variadic heterogeneous value type; fields `item_0`, `item_1`, …; `size()` |
| `string.cb` | `string` value type, manipulation, `IString` implementation |
| `list.cb` | `list<T>` - growable array; `add(move T)`, `get()`, `set(move T)`, `removeAt()`, `sort(comparator)` |
| `hashset.cb` | `hashset<T>` - open-addressed set; T must be integer-like |
| `dictionary.cb` | `dictionary<K,V>` - hash map; `add(K, move V)`, `set(K, move V)`, `get()`, `remove()` |
| `math.cb` | `Math` namespace: `abs`, `min`, `max`, `pow`, `sqrt`, `clamp`, trig, rounding |
| `stack.cb` | `stack<T>` - LIFO; `push()`, `pop()`, `peek()` |
| `queue.cb` | `queue<T>` - FIFO; `enqueue()`, `dequeue()`, `peek()` |
| `pair.cb` | `pair<A,B>` - two-field generic struct |
| `filesystem.cb` | `File.Exists()`, `File.ReadAllText()`, `File.WriteAllText()`, `File.move()` |
| `thread.cb` | `Thread` - Win32 thread wrapper; `start(fn, ctx)`, `join()` |
| `random.cb` | `Random` - splitmix64 PRNG; `next()`, `nextRange(min, max)` |
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
| `json.cb` | `JsonBuilder.toJson<T>`, `fromJson<T>` - JSON serialization/deserialization |

---
