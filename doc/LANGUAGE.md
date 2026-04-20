# Language & Core Library Features

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


## Memory Management & Filesystem (core/filesystem.cb)

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

## Other Features

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

The `if const` statement evaluates its condition at compile time, eliminating dead branches from the generated code:

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

This is useful for platform-specific code, feature gates, and performance-critical conditionals.

### Debug Info (Work in Progress)

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

See `core/` for standard library modules and more usage examples.
