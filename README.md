# C ♭(Flat)

A C-dialect language with modern features, compiled to LLVM IR.

CFlat (`.cb`) extends C with generics, interfaces, namespaces, operator overloading, and null-safety - while keeping C's familiar syntax and control.

## Quick Start

cflat runs natively on **Windows 10+** and **macOS (Apple Silicon)**, and is self-contained on each - it links native executables without an external toolchain: no Windows SDK or Visual Studio, no Xcode or Command Line Tools. Run `cflat --init` once after installing, and the hello app below builds and runs on a clean machine.

The compiler defaults to the native host target and picks the object format automatically; use `--platform` to cross-compile. See [`doc/CLI.md`](doc/CLI.md).

```bash
# Compile a source file to native executable
cflat hello.cb -o hello.exe
```

```c
import "list.cb";

extern int main() {
    list<int> nums;
    nums.add(42);
    printf("First: %d\n", nums.get(0));
    return 0;
}
```

## Examples

Each of these is a complete program - copy it into a `.cb` file and build it as above; the
trailing comments are what it prints. The full tour is in [`doc/LANGUAGE.md`](doc/LANGUAGE.md);
more programs live in [`example/`](example/).

### Interfaces and generics

An `interface` is dispatched through a vtable, and a generic container holds the interface
value itself - not a pointer to it. (A `class` is a `struct` that can implement interfaces.)

```c
import "list.cb";

interface IShape
{
    double area();
    string name();
};

class Circle : IShape
{
    double r = 0.0;
    double area() { return 3.14159 * r * r; }
    string name() { return "circle"; }
};

class Rect : IShape
{
    double w = 0.0;
    double h = 0.0;
    double area() { return w * h; }
    string name() { return "rect"; }
};

extern int main()
{
    list<IShape> shapes;

    Circle c; c.r = 2.0;            shapes.add(c);
    Rect r;   r.w = 3.0; r.h = 4.0; shapes.add(r);

    for (IShape s in shapes)
        printf("%s: %.2f\n", s.name().data(), s.area());   // circle: 12.57
                                                           // rect: 12.00
    return 0;
}
```

### JSON, with no boilerplate

`fromJson<T>` and `toJson<T>` are driven by compile-time introspection over the struct's
fields: no schema, no annotations, no generated code.

```c
import "json.cb";

struct Address {
    string city = default;
    int    zip  = 0;
};

struct User {
    string  name   = default;
    int     id     = 0;
    bool    active = false;
    Address addr   = default;
};

extern int main()
{
    string text = "{\"name\":\"Ada\",\"id\":42,\"active\":true,\"addr\":{\"city\":\"London\",\"zip\":12345}}";

    User u = JsonBuilder.fromJson<User>(text);
    printf("%s (#%d) from %s\n", u.name.data(), u.id, u.addr.city.data());

    u.id = 43;
    printf("%s\n", JsonBuilder.toJson<User>(u).data());
    return 0;
}

// Ada (#42) from London
// {"name":"Ada","id":43,"active":true,"addr":{"city":"London","zip":12345}}
```

### Null-safety

`?.` short-circuits the whole access when the target is null, and `??` supplies the fallback -
so a missing lookup can't dereference null.

```c
struct User {
    string name   = default;
    int    logins = 0;
    int    Read() { return logins; }
};

// Returns nullptr when the id is unknown.
User* find(User* table, int n, int id)
{
    for (int i = 0; i < n; i++)
        if (table[i].logins == id) return &table[i];
    return nullptr;
}

extern int main()
{
    User u;
    u.name = "ada";
    u.logins = 7;

    User* found   = find(&u, 1, 7);
    User* missing = find(&u, 1, 99);

    printf("%d\n", found?.logins ?? -1);    // 7  - non-null, reads the field
    printf("%d\n", missing?.logins ?? -1);  // -1 - null short-circuits to the fallback
    printf("%d\n", missing?.Read() ?? -1);  // -1 - method calls short-circuit too

    return 0;
}
```

### Real C, and the `program` construct

A `.c` file is compiled as real C by clang and linked in; its functions are auto-externed, so
no prototypes are needed. `import program` goes further and adopts a C `main` as a **program** -
a managed entry point that runs on its own thread with its own allocator, so everything it
allocates is freed when it returns. Its exit code comes back as a value. C interop links, so
build this one with `-o` rather than `--run`.

```c
// greet.c - plain C, compiled by clang and linked into the image
#include <stdio.h>
int main(int argc, char** argv) {
    for (int i = 0; i < argc; i++) printf("C sees argv[%d] = %s\n", i, argv[i]);
    return argc;
}
```

```c
// greeter.cb
import "list.cb";
import "thread.cb";
import program "greet.c" as Greet;

extern int main()
{
    Greet c;

    list<string> args;
    args.add("" + "hello");
    args.add("" + "world");

    c.run(args);
    c.WaitForExit();

    printf("C main returned argc=%d\n", c.exitCode);
    return 0;
}

// C sees argv[0] = hello
// C sees argv[1] = world
// C main returned argc=2
```

### VS Code Setup

1. Install the extension: in VS Code, open the Command Palette (`Ctrl+Shift+P`) -> **Extensions: Install from VSIX...** and pick `vscode-extension/cflat-language-*.vsix`.
2. Point the extension at your compiler: set [`cflat.executablePath`](vscode://settings/cflat.executablePath) to the full path of `cflat.exe`.

## Documentation

Start with **[Language & Core Library Features](doc/LANGUAGE.md)** - the complete language reference (types, functions, generics, interfaces, namespaces, memory management, null-safety, introspection, and compiler CLI options). It links out to the focused references: C interop, threading & memory, and HPC & SIMD.

- **[Contributing to cflat](doc/CONTRIBUTING.md)** - Building the compiler, development workflow, and running tests.
