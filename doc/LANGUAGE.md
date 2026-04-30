# Language & Core Library Features

## Table of Contents

- [Types](#types)
  - [Primitives](#primitives)
  - [Explicit-Width Integers](#explicit-width-integers)
  - [`string`](#string)
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
- [Interfaces & Polymorphism](#interfaces--polymorphism-coreinterfacescb)
  - [Defining and Implementing Interfaces](#defining-and-implementing-interfaces)
  - [Interface Parameters (VTable Dispatch)](#interface-parameters-vtable-dispatch)
  - [Generic Interfaces](#generic-interfaces)
  - [Interface Inheritance](#interface-inheritance)
  - [`is` and `as` Operators](#is-and-as-operators)
  - [Switch on Interface Type](#switch-on-interface-type)
- [Ownership / Lifetime (`move`)](#ownership--lifetime-move-keyword)
- [Namespaces & Modules](#namespaces--modules)
  - [Namespaces](#namespaces)
  - [`using` Aliases](#using-aliases)
  - [Module Imports](#module-imports)
- [Memory Management](#memory-management)
  - [`new` and `delete`](#new-and-delete)
  - [Custom Allocators](#custom-allocators-operator-new--operator-delete)
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
- [Other Features](#other-features)
  - [Operator Overloading](#operator-overloading)
  - [Bitwise and Shift Operators](#bitwise-and-shift-operators)
  - [Numeric Literals](#numeric-literals)
  - [Built-in Identifiers](#built-in-identifiers)
  - [Compile-Time Conditionals (`if const`)](#compile-time-conditionals-if-const)
- [Concurrency](#concurrency)
  - [Threads](#threads-corethread.cb)
  - [Cancellation](#cancellation-corestop_tokencb)
  - [Mutex](#mutex-coremutexcb)
  - [Atomic](#atomic-coreatomic.cb)
  - [Other Synchronization Primitives](#other-synchronization-primitives)
- [`program` Keyword](#program-keyword)
- [JSON](#json-libjsoncb)
- [Debug Info](#debug-info-work-in-progress)
- [Compiler CLI](#compiler-cli)
- [Standard Library](#standard-library)

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
s.data();     // i8* — null-terminated bytes
```

Concatenation with `operator+` — produces a freshly allocated buffer:

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

For incremental building, use `stringbuilder` — a mutable, growable buffer with `append`, `appendCStr`, `appendChar`, `clear`, and `toString`.

Define `operator string(T)` to convert arbitrary types to `string`; the core library provides conversions from `char*`, `i32`, and any `IString`.

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

Functions can be called before they are defined — no forward declarations needed. Mutual recursion works out of the box:

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

Anonymous functions use C#-style arrow syntax. Lambdas compile to named functions internally and are compatible with `function<T>`:

```c
function<int(int, int)> addL = (int a, int b) => { return a + b; };
addL(3, 4);   // 7

// Pass inline as argument
apply((int a, int b) => { return a * b; }, 5, 6);   // 30

// Zero-parameter lambda
function<int()> getK = () => { return 42; };

// Lambdas can contain full control flow
function<int(int)> sign = (int x) => {
    if (x > 0) return 1;
    if (x < 0) return -1;
    return 0;
};
```

---


## Generics/Templates & Ownership

Generics are resolved at compile time (monomorphization — no runtime overhead).

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
Square* sp = sc as Square;   // nullptr — sc is a Circle
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

The `move` keyword on a parameter definition transfers ownership into the callee — the resource is freed when the function returns:

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

- **Call site is silent** — no annotation required; the caller's pointer is automatically nulled after a `move` call.
- `move` is a soft keyword detected by text matching, not an ANTLR token — identifiers named `move` still work.
- For value types `move` is a no-op; ownership semantics only activate on pointer types.

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

---


## Memory Management

### `new` and `delete`

```c
class Node { int value = 0; };

Node* n   = new Node();
delete n;

int* arr  = new int[5];
delete[] arr;
```


### Custom Allocators (`operator new` / `operator delete`)

Override the global allocator by defining these operators:

```c
extern void* malloc(i64 size);
extern void  free(void* ptr);

void* operator new(long long size) { return malloc(size); }
void  operator delete(void* ptr)   { free(ptr); }
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

UFCS integer narrowing methods — each wraps to the target width:

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

Read annotation values at compile time with `annotationof(StructType, "fieldName", "AnnotationName")`. Returns a `string` — `"1"` for marker annotations, the serialized value for single-field annotations, or empty if the annotation is absent:

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

`reflect` also works on null pointers — calls `visitNull` with the field name.

---

## Range-Based For

Iterate over any type that provides `count()` and `get(int)` methods (including `list<T>`):

```c
import "list.cb";

list<int> nums;
nums.add(10);
nums.add(20);
nums.add(30);

for (int x in nums)
    printf("%d\n", x);
```

---

## Other Features

### Operator Overloading

Define `operator+`, `operator-`, `operator*`, `operator/`, `operator==`, `operator!=`, `operator<`, `operator>`, `operator[]`, `operator new`, `operator delete`, and `operator string` on structs:

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

## Concurrency

### Threads (`core/thread.cb`)

```c
import "thread.cb";

int worker(void* ctx)
{
    int* p = (int*)ctx;
    *p = 99;
    return 0;
}

int value = 0;
Thread t;
t.start(worker, &value);
t.join();
// value == 99
```

Pass lambdas directly to `start`:

```c
Thread t;
t.start((void* ctx) => { return 42; }, nullptr);
i32 code = t.join();   // 42
```

### Cancellation (`core/stop_token.cb`)

```c
import "stop_token.cb";

StopSource src;
StopToken token = src.token();

// In another thread: check token.isCancellationRequested()
src.cancel();
```

### Mutex (`core/mutex.cb`)

```c
import "mutex.cb";

Mutex m;
m.acquire();
// critical section
m.release();
```

Use the `lock` statement for scoped acquisition:

```c
lock (m)
{
    // automatically released at end of block
}
```

### Atomic (`core/atomic.cb`)

```c
import "atomic.cb";

Atomic<int> counter;
counter.store(0);
counter.fetchAdd(1);
int v = counter.load();
```

### Other Synchronization Primitives

- `core/semaphore.cb` — `Semaphore` with `acquire`/`release`
- `core/latch.cb` — `Latch` countdown; `countDown`, `wait`
- `core/rwlock.cb` — `RwLock`; `acquireRead`/`releaseRead`, `acquireWrite`/`releaseWrite`
- `core/channel.cb` — `channel<T>` blocking MPMC queue; `send`, `recv`, `tryRecv`
- `core/spsc_queue.cb` — `spsc_queue<T>` wait-free single-producer/single-consumer ring

```c
import "channel.cb";

channel<int> ch;
ch.send(42);
int v = ch.recv();   // 42
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
// {"full_name":"Alice","age":30}   — _token omitted

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
MyCompiler.exe app.cb -o app.ll -g
```

---

## Compiler CLI

```
MyCompiler.exe <input.cb> [options]
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
| `--help`                | Show usage and available options |

---

## Standard Library

`core/` is implicitly on the import search path. Only `runtime.cb` is auto-imported; all others require an explicit `import`.

| File | Exports |
|------|---------|
| `runtime.cb` | Allocator hooks (`new`, `delete`); exit/abort — **auto-imported** |
| `interfaces.cb` | `IString`, `IEnumerable<T>`, `IComparable<T>`, `IReflector` |
| `string.cb` | `string` value type, manipulation, `IString` implementation |
| `list.cb` | `list<T>` — growable array; `add(move T)`, `get()`, `set(move T)`, `removeAt()`, `sort(comparator)` |
| `hashset.cb` | `hashset<T>` — open-addressed set; T must be integer-like |
| `dictionary.cb` | `dictionary<K,V>` — hash map; `add(K, move V)`, `set(K, move V)`, `get()`, `remove()` |
| `math.cb` | `Math` namespace: `abs`, `min`, `max`, `pow`, `sqrt`, `clamp`, trig, rounding |
| `stack.cb` | `stack<T>` — LIFO; `push()`, `pop()`, `peek()` |
| `queue.cb` | `queue<T>` — FIFO; `enqueue()`, `dequeue()`, `peek()` |
| `pair.cb` | `pair<A,B>` — two-field generic struct |
| `filesystem.cb` | `File.Exists()`, `File.ReadAllText()`, `File.WriteAllText()`, `File.move()` |
| `thread.cb` | `Thread` — Win32 thread wrapper; `start(fn, ctx)`, `join()` |
| `random.cb` | `Random` — splitmix64 PRNG; `next()`, `nextRange(min, max)` |
| `time.cb` | `Duration`, `TimePoint`, `Stopwatch` — high-resolution timing |
| `mutex.cb` | `Mutex` — Win32 CRITICAL_SECTION wrapper |
| `atomic.cb` | `Atomic<T>` — load/store/fetchAdd with LLVM atomic IR |
| `semaphore.cb` | `Semaphore` — counting semaphore |
| `latch.cb` | `Latch` — countdown latch |
| `rwlock.cb` | `RwLock` — readers-writer lock |
| `channel.cb` | `channel<T>` — blocking MPMC queue |
| `spsc_queue.cb` | `spsc_queue<T>` — wait-free single-producer/single-consumer ring |
| `stop_token.cb` | `StopSource` / `StopToken` — cooperative cancellation |
| `arena.cb` | `Arena` — bump allocator |
| `json.cb` | `JsonBuilder.toJson<T>`, `fromJson<T>` — JSON serialization/deserialization |
