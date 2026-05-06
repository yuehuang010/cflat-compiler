import {
    Diagnostic,
    DiagnosticSeverity,
    CompletionItem,
    CompletionItemKind,
    InsertTextFormat,
    MarkupKind
} from 'vscode-languageserver/node';

// ---------------------------------------------------------------------------
// Types
// ---------------------------------------------------------------------------

export interface MemberInfo {
    name: string;
    type: string;
    isMethod: boolean;
    /** Display signature, e.g. "int count" or "void foo(int x)". */
    signature: string;
}

export interface SymbolInfo {
    kind: 'function' | 'type' | 'variable' | 'macro' | 'namespace';
    markdown: string;
    /** 0-based line of the declaration in the source document. */
    defLine: number;
    /** 0-based character offset of the symbol name on that line. */
    defChar: number;
    /** For variable symbols: the resolved base type name (e.g. "MyStruct"). */
    typeName?: string;
    /** For type symbols: the struct/class/union members available for member completion. */
    members?: MemberInfo[];
}

// ---------------------------------------------------------------------------
// Keyword documentation
// ---------------------------------------------------------------------------

export const KEYWORD_DOCS: Record<string, string> = {
    // Control flow
    'if':       '**if** *(control flow)*\n\nConditional branch. `if (condition) { ... }`',
    'else':     '**else** *(control flow)*\n\nFallback branch of an `if` statement.',
    'while':    '**while** *(control flow)*\n\nLoop while condition is true. `while (cond) { ... }`',
    'for':      '**for** *(control flow)*\n\nCounting loop. `for (init; cond; step) { ... }`',
    'do':       '**do** *(control flow)*\n\nDo-while loop. `do { ... } while (cond);`',
    'switch':   '**switch** *(control flow)*\n\nMulti-way branch. `switch (expr) { case N: ...; }`',
    'case':     '**case** *(control flow)*\n\nLabel inside a `switch` statement.',
    'default':  '**default** *(control flow)*\n\nFallback case in a `switch` statement.',
    'break':    '**break** *(control flow)*\n\nExit the nearest `switch`, `for`, `while`, or `do` loop.',
    'continue': '**continue** *(control flow)*\n\nSkip to the next iteration of the enclosing loop.',
    'return':   '**return** *(control flow)*\n\nReturn from the current function, optionally with a value.\n\nAlso supports the **return block** syntax: `return { field: value, ... };`',
    'goto':     '**goto** *(control flow)*\n\nUnconditional jump to a label.',

    // Types
    'void':     '**void** *(type)*\n\nThe empty type. Used as return type for functions that return nothing, or as `void*` for generic pointers.',
    'int':      '**int** *(type)*\n\nSigned 32-bit integer.',
    'char':     '**char** *(type)*\n\nSingle byte character / 8-bit integer.',
    'short':    '**short** *(type)*\n\nSigned 16-bit integer.',
    'long':     '**long** *(type)*\n\nSigned 64-bit integer (on 64-bit platforms).',
    'float':    '**float** *(type)*\n\n32-bit IEEE 754 floating-point number.',
    'double':   '**double** *(type)*\n\n64-bit IEEE 754 floating-point number.',
    'bool':     '**bool** *(type)*\n\nBoolean type. Values: `true`, `false`.',
    'unsigned': '**unsigned** *(type modifier)*\n\nUnsigned integer modifier.',
    'signed':   '**signed** *(type modifier)*\n\nSigned integer modifier (default for most integer types).',

    // Structs
    'struct':   '**struct** *(aggregate type)*\n\nDefine a named aggregate type with member fields and optional methods.\n\n```c\nstruct Point {\n    float x = 0;\n    float y = 0;\n    float length() { return x * x + y * y; }\n};\n```',
    'class':    '**class** *(alias for struct)*\n\nAlias for `struct` in MyC. Supports member variables with defaults, methods, and destructors.',
    'union':    '**union** *(aggregate type)*\n\nAll members share the same memory location.',
    'enum':     '**enum** *(enumeration type)*\n\nDefine a set of named integer constants.',

    // Storage class
    'static':   '**static** *(storage class)*\n\nFor local variables: persists across calls. For globals/functions: internal linkage.',
    'extern':   '**extern** *(storage class)*\n\nDeclare a symbol defined in another translation unit.',
    'typedef':  '**typedef** *(storage class)*\n\nCreate a type alias. `typedef int MyInt;`',
    'const':    '**const** *(qualifier)*\n\nMark a variable or pointer as read-only.',
    'inline':   '**inline** *(storage class)*\n\nHint to inline function calls at the call site.',
    'register':     '**register** *(storage class)*\n\nHint that the variable should be stored in a CPU register for fast access.',
    'volatile':     '**volatile** *(qualifier)*\n\nTell the compiler the value may change externally; prevents caching optimizations. Also used for the `volatile(...)` side-effect block.',
    'restrict':     '**restrict** *(qualifier)*\n\nPointer qualifier asserting that the pointed-to memory is not aliased.',
    'thread_local': '**thread_local** *(storage class)*\n\nEach thread has its own independent copy of the variable.',
    'auto':         '**auto** *(type inference)*\n\nInfer the variable type from its initializer.\n\n```c\nauto x = 42;      // int\nauto p = getPoint(); // Point\n```',
    'string':       '**string** *(built-in value type)*\n\nBuilt-in value type `{ i8* _ptr, i32 _len }`. String literals are automatically wrapped into a `string` by the compiler.\n\n**Members:**\n- `i8* data()` — pointer to the null-terminated bytes\n- `i32 length()` — number of bytes (not counting the null terminator)\n\n**Operators:**\n- `string operator+(string b)` — concatenate two strings (allocates a new buffer)\n- `string operator+(const char* b)` — concatenate with a raw C string\n\nSupports **format string** syntax — any `{expr}` inside a string literal is interpolated at runtime:\n\n```c\nstring s = "Hello, {name}! You have {count} items.";\n```\n\nNon-string expressions are coerced via `operator string`.',
    'stringbuilder': '**stringbuilder** *(struct)*\n\nMutable, growable string buffer defined in `core/string.cb`.\n\n**Members:**\n- `i8* data()` — pointer to the current buffer\n- `i32 length()` — current length in bytes\n- `string toString()` — return a `string` view over the current buffer (do not free the builder while the string is in use)\n- `void append(string s)` — append a `string` value\n- `void appendCStr(const char* s)` — append a raw C string\n- `void appendChar(i8 c)` — append a single character\n- `void clear()` — reset length to zero without freeing\n\n```c\nstringbuilder sb;\nsb.appendCStr("hello");\nsb.appendChar(\'!\');\nstring s = sb.toString();\n```',
    'IString':      '**IString** *(interface)*\n\nInterface for types that can be converted to a `string`.\n\n**Members:**\n- `string ToString()` — convert to a `string` value\n\nImplementors can be passed anywhere a `string operator string(IString* s)` conversion is accepted.',
    'alignas':      '**alignas** *(C11 alignment)*\n\nSpecify the alignment requirement of a variable or struct member.\n\n```c\nalignas(16) float vec[4];\nalignas(int) char buf[sizeof(int)];\n```',
    'stdcall':      '**stdcall** *(calling convention)*\n\nWindows x86 calling convention. The callee cleans the stack.\n\n```c\nvoid stdcall myFunc(int x);\n```',
    '?.': '**?.** *(null-conditional operator)*\n\nAccess a member or call a method only if the object is non-null. Returns zero/null if the object is null.\n\n```c\nint v = node?.value;       // field access\nint r = node?.Read();      // method call\n```',
    '??': '**??** *(null-coalescing operator)*\n\nReturn the left-hand side if it is non-zero/non-null, otherwise evaluate and return the right-hand side.\n\n```c\nint v = node?.value ?? -1;    // -1 if node is null\nconst char* s = name ?? \"unknown\";\n```',

    // CFlat extensions
    'namespace': '**namespace** *(CFlat extension)*\n\nGroup declarations under a named scope.\n\n```c\nnamespace Math {\n    float pi = 3.14159f;\n    float sqrt(float x) { ... }\n}\n```',
    'using':     '**using** *(CFlat extension)*\n\nImport a namespace into scope, or create a type alias.\n\n```c\n// Namespace import\nusing Math;\n// Type alias\nusing Vec2 = Math.Vector2;\n```',
    'import':    '**import** *(CFlat extension)*\n\nImport another source file into the current compilation unit.\n\n```c\nimport "utils.cb";\nimport "math/vector.cb";\n```',
    'interface': '**interface** *(CFlat extension)*\n\nDeclare an abstract set of method signatures that a struct must implement.\n\n```c\ninterface Drawable {\n    void draw();\n};\n```',
    'nameof':    '**nameof** *(CFlat extension)*\n\nReturn the name of a variable or type as a `const char*` string at compile time.\n\n```c\nconst char* name = nameof(myVariable);\n```',
    'typeof':    '**typeof** *(CFlat extension)*\n\nReturn the type name of an expression as a `const char*` string.\n\n```c\nconst char* typeName = typeof(myVar);\n```',
    'is':        '**is** *(CFlat extension)*\n\nType check operator. Tests whether an interface value holds a specific concrete type at runtime. Returns `bool`.\n\nThe type identity is stored in the fat pointer\'s vtable.\n\n```c\nIAnimal* a = getCat();\nif (a is Cat)\n{\n    // a holds a Cat\n}\n```',
    'as':        '**as** *(CFlat extension)*\n\nSafe cast operator. Extracts the concrete data pointer from an interface fat pointer if it matches the target type; returns `null` otherwise.\n\n```c\nIAnimal* a = getCat();\nCat* c = a as Cat;\nif (c != nullptr)\n{\n    c->purr();\n}\n```',
    'foreach': '**foreach** *(CFlat extension)*\n\nRange-based for loop. Calls `count()` and `get(int)` on the collection.\n\n```c\nforeach (string s in myList)\n{\n    printf("%s\\n", s.data());\n}\n```',
    'move':    '**move** *(CFlat extension — ownership transfer)*\n\nOn a **parameter definition**, transfers ownership of the pointer into the callee. The callee is responsible for freeing the resource. The caller\'s pointer is automatically nulled after the call.\n\n```c\nvoid consume(move Resource* r) { ... }   // owns r; freed on return\nvoid borrow(Resource* r)         { ... }   // caller still owns r\n```\n\n- `move` is a **soft keyword** — it is matched by text, not by the ANTLR lexer, so identifiers and methods named `move` are still valid.\n- No-op for value types (int, etc.).',
    'operator': '**operator** *(CFlat extension — operator overloading)*\n\nDefine a custom operator on a struct. The function must be a member of the struct.\n\n```c\nstruct Vec2 {\n    float x; float y;\n    Vec2 operator+(Vec2 b) { return { x + b.x, y + b.y }; }\n    bool operator==(Vec2 b) { return x == b.x && y == b.y; }\n    operator string() { return "({x}, {y})"; }\n};\n```\n\nSupported: `+`, `-`, `*`, `/`, `%`, `==`, `!=`, `<`, `>`, `<=`, `>=`, `new`, `delete`, `string` (implicit conversion).',

    // Compile-time macros
    '__FILE__':     '**__FILE__** *(compile-time macro)*\n\nExpands to the path of the current source file as a `const char*` string.',
    '__FUNCTION__': '**__FUNCTION__** *(compile-time macro)*\n\nExpands to the name of the enclosing function as a `const char*` string.',
    '__LINE__':     '**__LINE__** *(compile-time macro)*\n\nExpands to the current source line number as an `int`.',
    '__PLATFORM__': '**__PLATFORM__** *(compile-time macro)*\n\nExpands to `64` or `32` depending on the target platform (`-p win64` or `-p win32`). Use with `if const` to guard platform-specific code.\n\n```c\nif const (__PLATFORM__ == 64) {\n    // 64-bit path\n} else {\n    // 32-bit path\n}\n```',

    // Core collection types
    'list':       '**list\\<T\\>** *(core library — growable array)*\n\nDefined in `core/list.cb`. Automatically compiled into every program.\n\n**Members:**\n- `void add(move T value)` — append an element (takes ownership for pointer types)\n- `T get(int index)` — return element at index\n- `void set(int index, move T value)` — replace element\n- `void removeAt(int index)` — remove element (does NOT free owned pointers — caller must retrieve first)\n- `int count()` — current number of elements\n\n```c\nlist<int> nums;\nnums.add(1);\nnums.add(2);\nforeach (int n in nums) { printf("%d\\n", n); }\n```',
    'dictionary': '**dictionary\\<K,V\\>** *(core library — hash map)*\n\nDefined in `core/dictionary.cb`. Automatically compiled into every program.\n\n**Members:**\n- `void add(K key, move V value)` — insert (takes ownership of pointer V)\n- `void set(K key, move V value)` — insert or update\n- `V get(K key)` — return value for key\n- `bool remove(K key)` — remove key; returns true if found\n- `int count()` — number of entries',
    'hashset':    '**hashset\\<T\\>** *(core library — open-addressed set)*\n\nDefined in `core/hashset.cb`. T must be an integer-like type.\n\n**Members:**\n- `void add(T value)` — insert value\n- `bool contains(T value)` — membership test\n- `bool remove(T value)` — remove value\n- `int count()` — number of elements',
    'stack':      '**stack\\<T\\>** *(core library — LIFO)*\n\nDefined in `core/stack.cb`. Backed by a `list<T>`.\n\n**Members:**\n- `void push(T value)` — push onto top\n- `T pop()` — remove and return top element\n- `T peek()` — return top without removing\n- `int count()` — number of elements',
    'queue':      '**queue\\<T\\>** *(core library — FIFO)*\n\nDefined in `core/queue.cb`. Backed by a `list<T>`.\n\n**Members:**\n- `void enqueue(T value)` — add to back\n- `T dequeue()` — remove and return front element\n- `T peek()` — return front without removing\n- `int count()` — number of elements',
    'pair':       '**pair\\<A,B\\>** *(core library — two-field generic struct)*\n\nDefined in `core/pair.cb`.\n\n**Members:**\n- `A first` — first value\n- `B second` — second value',
    'Math':       '**Math** *(core library — math namespace)*\n\nDefined in `core/math.cb`. Automatically compiled into every program.\n\n**Members:**\n- `abs(x)`, `min(a,b)`, `max(a,b)`, `clamp(x,lo,hi)`\n- `pow(base,exp)`, `sqrt(x)`\n- `floor(x)`, `ceil(x)`, `round(x)`\n- `sin(x)`, `cos(x)`, `tan(x)`, `atan2(y,x)`',

    // C11
    'sizeof':         '**sizeof** *(operator)*\n\nReturn the size in bytes of a type or expression.',
    'alignof':        '**alignof** *(C11)*\n\nReturn the alignment requirement of a type.',
    '_Static_assert': '**_Static_assert** *(C11)*\n\nCompile-time assertion. `_Static_assert(sizeof(int) == 4, "int must be 4 bytes");`',
    'static_assert':  '**static_assert** *(C11)*\n\nCompile-time assertion (MyC lowercase alias). `static_assert(sizeof(int) == 4, "int must be 4 bytes");`',

    // Fixed-width integer types
    'i8':  '**i8** *(type)*\n\nSigned 8-bit integer. Equivalent to `signed char`.',
    'i16': '**i16** *(type)*\n\nSigned 16-bit integer. Equivalent to `short`.',
    'i32': '**i32** *(type)*\n\nSigned 32-bit integer. Equivalent to `int`.',
    'i64': '**i64** *(type)*\n\nSigned 64-bit integer. Equivalent to `long long`.',
    'u8':  '**u8** *(type)*\n\nUnsigned 8-bit integer. Equivalent to `unsigned char`.',
    'u16': '**u16** *(type)*\n\nUnsigned 16-bit integer. Equivalent to `unsigned short`.',
    'u32': '**u32** *(type)*\n\nUnsigned 32-bit integer. Equivalent to `unsigned int`.',
    'u64': '**u64** *(type)*\n\nUnsigned 64-bit integer. Equivalent to `unsigned long long`.',

    // Constants
    'true':    '**true** *(constant)*\n\nBoolean true (1).',
    'false':   '**false** *(constant)*\n\nBoolean false (0).',
    'NULL':    '**NULL** *(constant)*\n\nNull pointer constant.',
    'nullptr': '**nullptr** *(constant)*\n\nNull pointer constant (C++ style alias).'
};

// ---------------------------------------------------------------------------
// Built-in type symbols
// Injected into every document's symbol table so member completion and hover
// work even when core library definitions are not in the current file.
// ---------------------------------------------------------------------------

export const BUILTIN_TYPE_SYMBOLS: Map<string, SymbolInfo> = new Map([
    ['string', {
        kind: 'type',
        markdown: KEYWORD_DOCS['string'],
        defLine: 0,
        defChar: 0,
        members: [
            { name: 'data',   type: 'i8*', isMethod: true,  signature: 'i8* data()' },
            { name: 'length', type: 'i32', isMethod: true,  signature: 'i32 length()' },
            { name: '_ptr',   type: 'i8*', isMethod: false, signature: 'i8* _ptr' },
            { name: '_len',   type: 'i32', isMethod: false, signature: 'i32 _len' },
        ]
    }],
    ['stringbuilder', {
        kind: 'type',
        markdown: KEYWORD_DOCS['stringbuilder'],
        defLine: 0,
        defChar: 0,
        members: [
            { name: 'data',       type: 'i8*',    isMethod: true,  signature: 'i8* data()' },
            { name: 'length',     type: 'i32',    isMethod: true,  signature: 'i32 length()' },
            { name: 'toString',   type: 'string', isMethod: true,  signature: 'string toString()' },
            { name: 'append',     type: 'void',   isMethod: true,  signature: 'void append(string s)' },
            { name: 'appendCStr', type: 'void',   isMethod: true,  signature: 'void appendCStr(const char* s)' },
            { name: 'appendChar', type: 'void',   isMethod: true,  signature: 'void appendChar(i8 c)' },
            { name: 'clear',      type: 'void',   isMethod: true,  signature: 'void clear()' },
            { name: '_data',      type: 'i8*',    isMethod: false, signature: 'i8* _data' },
            { name: '_length',    type: 'i32',    isMethod: false, signature: 'i32 _length' },
            { name: '_capacity',  type: 'i32',    isMethod: false, signature: 'i32 _capacity' },
        ]
    }],
    ['IString', {
        kind: 'type',
        markdown: KEYWORD_DOCS['IString'],
        defLine: 0,
        defChar: 0,
        members: [
            { name: 'ToString', type: 'string', isMethod: true, signature: 'string ToString()' },
        ]
    }],
    ['list', {
        kind: 'type',
        markdown: KEYWORD_DOCS['list'],
        defLine: 0,
        defChar: 0,
        members: [
            { name: 'add',      type: 'void', isMethod: true,  signature: 'void add(T value)' },
            { name: 'get',      type: 'T',    isMethod: true,  signature: 'T get(int index)' },
            { name: 'set',      type: 'void', isMethod: true,  signature: 'void set(int index, T value)' },
            { name: 'removeAt', type: 'void', isMethod: true,  signature: 'void removeAt(int index)' },
            { name: 'count',    type: 'int',  isMethod: true,  signature: 'int count()' },
        ]
    }],
    ['dictionary', {
        kind: 'type',
        markdown: KEYWORD_DOCS['dictionary'],
        defLine: 0,
        defChar: 0,
        members: [
            { name: 'add',    type: 'void', isMethod: true, signature: 'void add(K key, V value)' },
            { name: 'set',    type: 'void', isMethod: true, signature: 'void set(K key, V value)' },
            { name: 'get',    type: 'V',    isMethod: true, signature: 'V get(K key)' },
            { name: 'remove', type: 'bool', isMethod: true, signature: 'bool remove(K key)' },
            { name: 'count',  type: 'int',  isMethod: true, signature: 'int count()' },
        ]
    }],
    ['hashset', {
        kind: 'type',
        markdown: KEYWORD_DOCS['hashset'],
        defLine: 0,
        defChar: 0,
        members: [
            { name: 'add',      type: 'void', isMethod: true, signature: 'void add(T value)' },
            { name: 'contains', type: 'bool', isMethod: true, signature: 'bool contains(T value)' },
            { name: 'remove',   type: 'bool', isMethod: true, signature: 'bool remove(T value)' },
            { name: 'count',    type: 'int',  isMethod: true, signature: 'int count()' },
        ]
    }],
    ['stack', {
        kind: 'type',
        markdown: KEYWORD_DOCS['stack'],
        defLine: 0,
        defChar: 0,
        members: [
            { name: 'push',  type: 'void', isMethod: true, signature: 'void push(T value)' },
            { name: 'pop',   type: 'T',    isMethod: true, signature: 'T pop()' },
            { name: 'peek',  type: 'T',    isMethod: true, signature: 'T peek()' },
            { name: 'count', type: 'int',  isMethod: true, signature: 'int count()' },
        ]
    }],
    ['queue', {
        kind: 'type',
        markdown: KEYWORD_DOCS['queue'],
        defLine: 0,
        defChar: 0,
        members: [
            { name: 'enqueue', type: 'void', isMethod: true, signature: 'void enqueue(T value)' },
            { name: 'dequeue', type: 'T',    isMethod: true, signature: 'T dequeue()' },
            { name: 'peek',    type: 'T',    isMethod: true, signature: 'T peek()' },
            { name: 'count',   type: 'int',  isMethod: true, signature: 'int count()' },
        ]
    }],
    ['pair', {
        kind: 'type',
        markdown: KEYWORD_DOCS['pair'],
        defLine: 0,
        defChar: 0,
        members: [
            { name: 'first',  type: 'A', isMethod: false, signature: 'A first' },
            { name: 'second', type: 'B', isMethod: false, signature: 'B second' },
        ]
    }],
    ['Math', {
        kind: 'namespace',
        markdown: KEYWORD_DOCS['Math'],
        defLine: 0,
        defChar: 0,
        members: [
            { name: 'abs',   type: 'double', isMethod: true, signature: 'double abs(double x)' },
            { name: 'min',   type: 'double', isMethod: true, signature: 'double min(double a, double b)' },
            { name: 'max',   type: 'double', isMethod: true, signature: 'double max(double a, double b)' },
            { name: 'clamp', type: 'double', isMethod: true, signature: 'double clamp(double x, double lo, double hi)' },
            { name: 'pow',   type: 'double', isMethod: true, signature: 'double pow(double base, double exp)' },
            { name: 'sqrt',  type: 'double', isMethod: true, signature: 'double sqrt(double x)' },
            { name: 'floor', type: 'double', isMethod: true, signature: 'double floor(double x)' },
            { name: 'ceil',  type: 'double', isMethod: true, signature: 'double ceil(double x)' },
            { name: 'round', type: 'double', isMethod: true, signature: 'double round(double x)' },
            { name: 'sin',   type: 'double', isMethod: true, signature: 'double sin(double x)' },
            { name: 'cos',   type: 'double', isMethod: true, signature: 'double cos(double x)' },
            { name: 'tan',   type: 'double', isMethod: true, signature: 'double tan(double x)' },
            { name: 'atan2', type: 'double', isMethod: true, signature: 'double atan2(double y, double x)' },
        ]
    }],
]);

// ---------------------------------------------------------------------------
// Comment stripping
// ---------------------------------------------------------------------------

/** Strip line and block comments, preserving newlines so line numbers stay intact. */
export function removeComments(text: string): string {
    return text
        .replace(/\/\*[\s\S]*?\*\//g, m => m.replace(/[^\n]/g, ' '))
        .replace(/\/\/[^\n]*/g, m => ' '.repeat(m.length));
}

// ---------------------------------------------------------------------------
// Struct member extraction
// ---------------------------------------------------------------------------

/** Extract members (fields + methods) from a struct/class body string. */
export function extractStructMembers(body: string): MemberInfo[] {
    const members: MemberInfo[] = [];
    let depth = 0;
    let segStart = 0;

    const addMember = (segment: string) => {
        const norm = segment.replace(/\s+/g, ' ').trim().replace(/[;{}]$/, '').trim();
        if (!norm || norm.startsWith('#') || norm.length > 150) return;

        if (norm.includes('(')) {
            const parenOpen  = norm.indexOf('(');
            const parenClose = norm.indexOf(')');
            if (parenClose === -1) return;
            const beforeParen = norm.slice(0, parenOpen).trim();
            const nameParts   = beforeParen.split(/\s+/);
            const name        = nameParts[nameParts.length - 1].replace(/[*&]/g, '');
            const type        = nameParts.slice(0, nameParts.length - 1).join(' ') || 'void';
            const signature   = norm.slice(0, parenClose + 1);
            if (name) members.push({ name, type, isMethod: true, signature });
        } else {
            const normField = norm.replace(/\s*=.*$/, '').trim();
            const parts = normField.split(/\s+/);
            const rawName = parts[parts.length - 1].replace(/\[.*?\]$/, '').replace(/[*&]/g, '');
            const type    = parts.slice(0, parts.length - 1).join(' ');
            if (rawName && type) members.push({ name: rawName, type, isMethod: false, signature: `${type} ${rawName}` });
        }
    };

    for (let j = 0; j < body.length; j++) {
        const ch = body[j];
        if (ch === '{') {
            depth++;
        } else if (ch === '}') {
            depth--;
            if (depth === 0) {
                addMember(body.slice(segStart, j + 1));
                segStart = j + 1;
            }
        } else if (ch === ';' && depth === 0) {
            addMember(body.slice(segStart, j));
            segStart = j + 1;
        }
    }

    return members;
}

// ---------------------------------------------------------------------------
// Position utilities
// ---------------------------------------------------------------------------

/** Convert a character index in `text` to a 0-based { line, character } position. */
export function indexToPosition(text: string, index: number): { line: number; character: number } {
    const before = text.slice(0, index);
    const lines = before.split('\n');
    return { line: lines.length - 1, character: lines[lines.length - 1].length };
}

/** Extract the word (identifier) surrounding `offset` in `text`. Returns null if none. */
export function getWordAt(text: string, offset: number): string | null {
    if (offset < 0 || offset >= text.length || !/\w/.test(text[offset])) return null;
    let start = offset;
    let end = offset;
    while (start > 0 && /\w/.test(text[start - 1])) start--;
    while (end < text.length && /\w/.test(text[end])) end++;
    return start === end ? null : text.slice(start, end);
}

// ---------------------------------------------------------------------------
// Parameter extraction
// ---------------------------------------------------------------------------

/** Words that cannot be C type names (control-flow keywords, etc.). */
export const CANNOT_BE_TYPE = new Set([
    'if', 'else', 'while', 'for', 'foreach', 'do', 'switch', 'case', 'default',
    'break', 'continue', 'return', 'goto', 'using', 'namespace', 'interface',
    'sizeof', 'typeof', 'nameof', 'alignof', 'import', 'alignas', 'volatile', 'stdcall',
    'is', 'as', 'move', 'operator'
]);

/**
 * Parse a raw function parameter-list string into { name, fullType, typeName } triples.
 * Splits on commas that are NOT inside angle brackets so generic types like
 * Map<int,int> are kept intact.
 */
export function extractParams(paramStr: string): Array<{ name: string; fullType: string; typeName: string }> {
    if (!paramStr) return [];

    const parts: string[] = [];
    let depth = 0, segStart = 0;
    for (let i = 0; i < paramStr.length; i++) {
        if      (paramStr[i] === '<') depth++;
        else if (paramStr[i] === '>') depth--;
        else if (paramStr[i] === ',' && depth === 0) {
            parts.push(paramStr.slice(segStart, i).trim());
            segStart = i + 1;
        }
    }
    parts.push(paramStr.slice(segStart).trim());

    const nameRe = /^(.*[^\w])([A-Za-z_]\w*)(\s*\[.*?\])?\s*$/;
    const result: Array<{ name: string; fullType: string; typeName: string }> = [];

    for (const part of parts) {
        const p = part.replace(/=.*$/, '').trim();
        if (!p || p === '...' || p === 'void') continue;

        const m = nameRe.exec(p);
        if (!m || !m[1].trim()) continue;

        const rawTypePart = m[1].trim();
        const name        = m[2];
        if (KEYWORD_DOCS[name] || CANNOT_BE_TYPE.has(name)) continue;

        const ptrMatch = rawTypePart.match(/(\*+)$/);
        const ptrStars = ptrMatch ? ptrMatch[1] : '';
        const baseType = ptrStars ? rawTypePart.slice(0, -ptrStars.length).trim() : rawTypePart;
        const fullType = ptrStars ? `${baseType}${ptrStars}` : baseType;
        const typeName = baseType.replace(/<[^>]*>/g, '').trim();
        result.push({ name, fullType, typeName });
    }
    return result;
}

// ---------------------------------------------------------------------------
// Document symbol extraction
// ---------------------------------------------------------------------------

/**
 * Scan a document and produce a symbol table for user-defined identifiers.
 * Handles: #define macros, namespaces, structs/classes/unions/enums (with
 * member lists), interfaces, function declarations, and variable declarations.
 */
export function parseDocumentSymbols(text: string): Map<string, SymbolInfo> {
    const symbols = new Map<string, SymbolInfo>();
    const clean = removeComments(text);

    // --- #define macros ---
    const defineRe = /#\s*define\s+([A-Za-z_]\w*)(?:\(([^)]*)\))?\s*(.*)/g;
    let m: RegExpExecArray | null;
    while ((m = defineRe.exec(clean)) !== null) {
        const name = m[1];
        const params = m[2];
        const value = m[3].trim();
        const sig = params !== undefined
            ? `#define ${name}(${params})${value ? ' ' + value : ''}`
            : `#define ${name}${value ? ' ' + value : ''}`;
        const nameIndex = m.index + m[0].indexOf(name);
        const pos = indexToPosition(clean, nameIndex);
        symbols.set(name, {
            kind: 'macro',
            markdown: `\`\`\`c\n${sig}\n\`\`\``,
            defLine: pos.line,
            defChar: pos.character
        });
    }

    // --- namespace declarations ---
    const nsRe = /\bnamespace\s+([A-Za-z_]\w*)/g;
    while ((m = nsRe.exec(clean)) !== null) {
        const name = m[1];
        if (!symbols.has(name)) {
            const nameIndex = m.index + m[0].indexOf(name);
            const pos = indexToPosition(clean, nameIndex);
            symbols.set(name, {
                kind: 'namespace',
                markdown: `\`\`\`c\nnamespace ${name} { ... }\n\`\`\``,
                defLine: pos.line,
                defChar: pos.character
            });
        }
    }

    // --- type aliases: using Alias = Target; ---
    const aliasRe = /\busing\s+([A-Za-z_]\w*)\s*=\s*([A-Za-z_]\w*(?:\.[A-Za-z_]\w*)*)\s*;/g;
    while ((m = aliasRe.exec(clean)) !== null) {
        const alias = m[1];
        const fullTarget = m[2];
        const targetBase = fullTarget.split('.').pop()!;
        const nameIndex = m.index + m[0].indexOf(alias);
        const pos = indexToPosition(clean, nameIndex);
        if (!symbols.has(alias)) {
            symbols.set(alias, {
                kind: 'type',
                markdown: `\`\`\`c\nusing ${alias} = ${fullTarget};\n\`\`\``,
                defLine: pos.line,
                defChar: pos.character,
                typeName: targetBase
            });
        }
    }

    // Helper: extract body + position for an aggregate type.
    const extractAggregate = (keyword: string, name: string, bodyStart: number, nameGlobalIndex: number): void => {
        let depth = 1;
        let i = bodyStart;
        while (i < clean.length && depth > 0) {
            if (clean[i] === '{') depth++;
            else if (clean[i] === '}') depth--;
            i++;
        }
        const body = clean.slice(bodyStart, i - 1);
        const members = extractStructMembers(body);
        const memberLines = members.map(mb => `    ${mb.signature};`);
        const md = memberLines.length > 0
            ? `\`\`\`c\n${keyword} ${name} {\n${memberLines.join('\n')}\n}\n\`\`\``
            : `\`\`\`c\n${keyword} ${name}\n\`\`\``;
        const pos = indexToPosition(clean, nameGlobalIndex);
        symbols.set(name, { kind: 'type', markdown: md, defLine: pos.line, defChar: pos.character, members });
    };

    // --- struct / class / union / enum ---
    const aggRe = /\b(struct|class|union|enum)\s+([A-Za-z_]\w*)[^;{]*\{/g;
    while ((m = aggRe.exec(clean)) !== null) {
        const nameIndex = m.index + m[0].indexOf(m[2]);
        extractAggregate(m[1], m[2], m.index + m[0].length, nameIndex);
    }

    // --- interface (CFlat extension) ---
    const ifaceRe = /\binterface\s+([A-Za-z_]\w*)[^;{]*\{/g;
    while ((m = ifaceRe.exec(clean)) !== null) {
        const nameIndex = m.index + m[0].indexOf(m[1]);
        extractAggregate('interface', m[1], m.index + m[0].length, nameIndex);
    }

    // Function: [qualifiers] returnType[*] qualName(params) [const] [{;]
    const funcLineRe = /^([ \t]*(?:(?:static|extern|inline|virtual)\s+)*)((?:const\s+)?(?:(?:unsigned|signed|long|short)\s+)*(?:(?:struct|class|union|enum)\s+)?[A-Za-z_]\w*(?:\s*<[^>]*>)?(?:\s*\*+)?)\s+([A-Za-z_][\w:,]*)\s*\(([^)]*)\)(?:\s*const)?\s*[{;]?/;
    // Variable: [qualifiers] type[*] name [array] [= ...];
    const varLineRe   = /^([ \t]*(?:(?:const|volatile|static|extern|register)\s+)*)((?:(?:unsigned|signed|long|short)\s+)*(?:(?:struct|class|union|enum)\s+)?[A-Za-z_]\w*(?:\s*<[^>]*>)?)(\s*\*+\s*|\s+)([A-Za-z_]\w*)\s*(?:\[.*?\])?\s*(?:=.*)?\s*;$/;
    // For-loop variable: for (type name = ...)
    const forVarRe    = /\bfor\s*\(\s*((?:(?:const|volatile)\s+)?(?:(?:unsigned|signed|long|short)\s+)*(?:(?:struct|class|union|enum)\s+)?[A-Za-z_]\w*(?:\s*<[^>]*>)?)(\s*\*+\s*|\s+)([A-Za-z_]\w*)\s*=/g;
    // Foreach-loop variable: foreach (Type name in collection)
    const foreachVarRe = /\bforeach\s*\(\s*((?:(?:const|volatile)\s+)?(?:(?:unsigned|signed|long|short)\s+)*(?:(?:struct|class|union|enum)\s+)?[A-Za-z_]\w*(?:\s*<[^>]*>)?)(\s*\*+\s*|\s+)([A-Za-z_]\w*)\s+in\b/g;

    const addVarSymbol = (name: string, fullType: string, lineIdx: number, defChar: number) => {
        if (!symbols.has(name) && !KEYWORD_DOCS[name]) {
            const typeName = fullType.replace(/\*+$/, '').replace(/<[^>]*>/g, '').trim();
            symbols.set(name, {
                kind: 'variable',
                markdown: `\`\`\`c\n${fullType} ${name}\n\`\`\``,
                defLine: lineIdx,
                defChar: Math.max(0, defChar),
                typeName
            });
        }
    };

    const pendingAutos: Array<{ name: string; rhsIdent: string; lineIdx: number; defChar: number }> = [];

    const lines = clean.split('\n');
    for (let lineIdx = 0; lineIdx < lines.length; lineIdx++) {
        const rawLine = lines[lineIdx].trimEnd();

        // --- Function pattern ---
        const fm = funcLineRe.exec(rawLine);
        if (fm) {
            const retType = fm[2].trim();
            const qualName = fm[3];
            const shortName = qualName.split('::').pop()!;
            const params = fm[4].trim();

            if (!KEYWORD_DOCS[shortName] && !CANNOT_BE_TYPE.has(retType.split(/\s+/)[0])) {
                const parenIdx = rawLine.indexOf('(');
                const beforeParen = parenIdx > 0 ? rawLine.slice(0, parenIdx) : '';
                const defChar = Math.max(0, beforeParen.lastIndexOf(shortName));

                const info: SymbolInfo = {
                    kind: 'function',
                    markdown: `\`\`\`c\n${retType} ${shortName}(${params})\n\`\`\``,
                    defLine: lineIdx,
                    defChar,
                    typeName: retType.replace(/\*+$/, '').replace(/<[^>]*>/g, '').trim()
                };
                if (!symbols.has(shortName)) symbols.set(shortName, info);
                if (qualName !== shortName && !symbols.has(qualName)) symbols.set(qualName, info);
            }

            for (const { name, fullType, typeName } of extractParams(params)) {
                if (!symbols.has(name)) {
                    symbols.set(name, {
                        kind: 'variable',
                        markdown: `\`\`\`c\n${fullType} ${name}\n\`\`\``,
                        defLine: lineIdx,
                        defChar: 0,
                        typeName
                    });
                }
            }
            continue;
        }

        // --- Variable pattern ---
        const vm = varLineRe.exec(rawLine);
        if (vm) {
            const baseType = vm[2].trim();
            const sep      = vm[3];
            const name     = vm[4];
            const ptrStars = sep.trim();
            const fullType = ptrStars ? `${baseType}${ptrStars}` : baseType;

            if (baseType === 'auto') {
                const typeEnd = vm[1].length + vm[2].length + vm[3].length;
                const defChar = vm[0].indexOf(name, typeEnd);

                const asMatch = /\bas\s+([A-Za-z_]\w*(?:\.[A-Za-z_]\w*)*)/.exec(rawLine);
                const isMatch = /\bis\s+([A-Za-z_]\w*(?:\.[A-Za-z_]\w*)*)/.exec(rawLine);

                if (asMatch && !KEYWORD_DOCS[name]) {
                    addVarSymbol(name, asMatch[1] + '*', lineIdx, defChar);
                } else if (isMatch && !KEYWORD_DOCS[name]) {
                    addVarSymbol(name, 'bool', lineIdx, defChar);
                } else {
                    const rhsMatch = /=\s*(?:new\s+)?([A-Za-z_]\w*(?:\.[A-Za-z_]\w*)*)/.exec(rawLine);
                    if (rhsMatch && !KEYWORD_DOCS[name]) {
                        pendingAutos.push({ name, rhsIdent: rhsMatch[1], lineIdx, defChar });
                    } else if (!KEYWORD_DOCS[name]) {
                        addVarSymbol(name, 'auto', lineIdx, defChar);
                    }
                }
            } else if (!CANNOT_BE_TYPE.has(baseType.split(/\s+/)[0])) {
                const typeEnd = vm[1].length + vm[2].length + vm[3].length;
                addVarSymbol(name, fullType, lineIdx, vm[0].indexOf(name, typeEnd));
            }
        }

        // --- For-loop variable: for (int i = 0; ...) ---
        forVarRe.lastIndex = 0;
        let fv: RegExpExecArray | null;
        while ((fv = forVarRe.exec(rawLine)) !== null) {
            const baseType = fv[1].trim();
            const sep      = fv[2];
            const name     = fv[3];
            const ptrStars = sep.trim();
            const fullType = ptrStars ? `${baseType}${ptrStars}` : baseType;

            if (!CANNOT_BE_TYPE.has(baseType.split(/\s+/)[0])) {
                const nameIdx = fv.index + fv[0].lastIndexOf(name);
                addVarSymbol(name, fullType, lineIdx, nameIdx);
            }
        }

        // --- Foreach-loop variable: foreach (Type name in collection) ---
        foreachVarRe.lastIndex = 0;
        let fe: RegExpExecArray | null;
        while ((fe = foreachVarRe.exec(rawLine)) !== null) {
            const baseType = fe[1].trim();
            const sep      = fe[2];
            const name     = fe[3];
            const ptrStars = sep.trim();
            const fullType = ptrStars ? `${baseType}${ptrStars}` : baseType;

            if (!CANNOT_BE_TYPE.has(baseType.split(/\s+/)[0])) {
                const nameIdx = fe.index + fe[0].lastIndexOf(name);
                addVarSymbol(name, fullType, lineIdx, nameIdx);
            }
        }
    }

    // --- Resolve deferred auto-typed variables ---
    for (const { name, rhsIdent, lineIdx, defChar } of pendingAutos) {
        if (symbols.has(name) || KEYWORD_DOCS[name]) continue;

        let rhsInfo = symbols.get(rhsIdent);
        if (!rhsInfo && rhsIdent.includes('.')) {
            const shortName = rhsIdent.split('.').pop();
            if (shortName) rhsInfo = symbols.get(shortName);
        }

        let resolvedType: string | undefined;
        let members: MemberInfo[] | undefined;
        if (rhsInfo?.kind === 'type') {
            resolvedType = rhsIdent;
            members = rhsInfo.members;
        } else if (rhsInfo?.kind === 'variable' || rhsInfo?.kind === 'function') {
            resolvedType = rhsInfo.typeName;
            if (resolvedType) {
                const resolvedTypeInfo = symbols.get(resolvedType);
                if (resolvedTypeInfo?.kind === 'type') {
                    members = resolvedTypeInfo.members;
                }
            }
        }
        const displayType = resolvedType ?? 'auto';
        symbols.set(name, {
            kind: 'variable',
            markdown: `\`\`\`c\n${displayType} ${name}\n\`\`\``,
            defLine: lineIdx,
            defChar: Math.max(0, defChar),
            typeName: resolvedType,
            members
        });
    }

    // --- Resolve type alias members ---
    for (const [, info] of symbols) {
        if (info.kind === 'type' && info.typeName && !info.members) {
            const targetInfo = symbols.get(info.typeName);
            if (targetInfo?.kind === 'type' && targetInfo.members) {
                info.members = targetInfo.members;
            }
        }
    }

    // --- Inject built-in types ---
    for (const [name, info] of BUILTIN_TYPE_SYMBOLS) {
        if (!symbols.has(name)) {
            symbols.set(name, info);
        }
    }

    // --- Propagate built-in type members to variables ---
    for (const [, info] of symbols) {
        if ((info.kind === 'variable' || info.kind === 'function') && info.typeName && !info.members) {
            const typeInfo = symbols.get(info.typeName);
            if (typeInfo?.kind === 'type' && typeInfo.members) {
                info.members = typeInfo.members;
            }
        }
    }

    return symbols;
}

// ---------------------------------------------------------------------------
// Compiler output parsing
// ---------------------------------------------------------------------------

/**
 * Parse compiler diagnostic output.
 *
 * Handles these formats:
 *   ANTLR4:   line 5:10 no viable alternative at input 'foo'
 *   LLVM/GCC: filename:5:10: error: message
 *             filename:5:10: warning: message
 *   Generic:  error: message  (no position)
 */
export function parseCompilerOutput(output: string, lineCount: number): Diagnostic[] {
    const diagnostics: Diagnostic[] = [];

    const antlr4Pattern = /^line (\d+):(\d+)\s+(.+)$/;
    const gccPattern = /^(?:[^:]+):(\d+):(\d+):\s*(error|warning|note):\s*(.+)$/;
    const genericErrorPattern = /^(error|fatal error):\s*(.+)$/i;

    for (const line of output.split('\n')) {
        const trimmed = line.trim();
        if (!trimmed) continue;

        let match: RegExpMatchArray | null;

        // ANTLR4 format: line N:M message
        if ((match = antlr4Pattern.exec(trimmed))) {
            const lineNum = Math.max(0, parseInt(match[1]) - 1);
            const colNum = Math.max(0, parseInt(match[2]));
            const message = match[3].trim();

            if (lineNum < lineCount) {
                diagnostics.push({
                    severity: DiagnosticSeverity.Error,
                    range: {
                        start: { line: lineNum, character: colNum },
                        end: { line: lineNum, character: colNum + 1 }
                    },
                    message,
                    source: 'cflat'
                });
            }
            continue;
        }

        // GCC/LLVM format: file:line:col: severity: message
        if ((match = gccPattern.exec(trimmed))) {
            const lineNum = Math.max(0, parseInt(match[1]) - 1);
            const colNum = Math.max(0, parseInt(match[2]) - 1);
            const severity = match[3].toLowerCase();
            const message = match[4].trim();

            const diaSeverity =
                severity === 'warning' ? DiagnosticSeverity.Warning :
                severity === 'note'    ? DiagnosticSeverity.Information :
                                         DiagnosticSeverity.Error;

            if (lineNum < lineCount) {
                diagnostics.push({
                    severity: diaSeverity,
                    range: {
                        start: { line: lineNum, character: colNum },
                        end: { line: lineNum, character: colNum + 1 }
                    },
                    message,
                    source: 'cflat'
                });
            }
            continue;
        }

        // Generic error with no position
        if ((match = genericErrorPattern.exec(trimmed))) {
            diagnostics.push({
                severity: DiagnosticSeverity.Error,
                range: {
                    start: { line: 0, character: 0 },
                    end: { line: 0, character: 1 }
                },
                message: match[2].trim(),
                source: 'cflat'
            });
        }
    }

    return diagnostics;
}

// ---------------------------------------------------------------------------
// Completion items
// ---------------------------------------------------------------------------

export function makeKeyword(label: string, detail?: string): CompletionItem {
    return { label, kind: CompletionItemKind.Keyword, detail: detail ?? 'keyword' };
}

export function makeSnippet(label: string, insertText: string, detail: string): CompletionItem {
    return {
        label,
        kind: CompletionItemKind.Snippet,
        insertText,
        insertTextFormat: InsertTextFormat.Snippet,
        detail
    };
}

export const COMPLETION_ITEMS: CompletionItem[] = [
    // Control flow keywords
    makeKeyword('if'),
    makeKeyword('else'),
    makeKeyword('while'),
    makeKeyword('for'),
    makeKeyword('do'),
    makeKeyword('switch'),
    makeKeyword('case'),
    makeKeyword('default'),
    makeKeyword('break'),
    makeKeyword('continue'),
    makeKeyword('return'),
    makeKeyword('goto'),

    // Types
    makeKeyword('void', 'type'),
    makeKeyword('int', 'type'),
    makeKeyword('char', 'type'),
    makeKeyword('short', 'type'),
    makeKeyword('long', 'type'),
    makeKeyword('float', 'type'),
    makeKeyword('double', 'type'),
    makeKeyword('bool', 'type'),
    makeKeyword('string', 'type — built-in string'),
    makeKeyword('unsigned', 'type modifier'),
    makeKeyword('signed', 'type modifier'),

    // Aggregate types
    makeKeyword('struct', 'aggregate type'),
    makeKeyword('class', 'aggregate type (alias for struct)'),
    makeKeyword('union', 'aggregate type'),
    makeKeyword('enum', 'enumeration type'),

    // Storage class
    makeKeyword('static', 'storage class'),
    makeKeyword('extern', 'storage class'),
    makeKeyword('typedef', 'type alias'),
    makeKeyword('const', 'qualifier'),
    makeKeyword('inline', 'inline hint'),
    makeKeyword('register', 'storage class'),
    makeKeyword('volatile', 'qualifier'),
    makeKeyword('restrict', 'qualifier'),
    makeKeyword('thread_local', 'storage class'),
    makeKeyword('alignas', 'C11 alignment specifier'),
    makeKeyword('stdcall', 'calling convention'),

    // Fixed-width integer types
    makeKeyword('i8',  'type — signed 8-bit integer'),
    makeKeyword('i16', 'type — signed 16-bit integer'),
    makeKeyword('i32', 'type — signed 32-bit integer'),
    makeKeyword('i64', 'type — signed 64-bit integer'),
    makeKeyword('u8',  'type — unsigned 8-bit integer'),
    makeKeyword('u16', 'type — unsigned 16-bit integer'),
    makeKeyword('u32', 'type — unsigned 32-bit integer'),
    makeKeyword('u64', 'type — unsigned 64-bit integer'),

    // CFlat extensions
    makeKeyword('namespace', 'CFlat extension'),
    makeKeyword('using', 'CFlat extension'),
    makeKeyword('import', 'CFlat extension'),
    makeKeyword('interface', 'CFlat extension'),
    makeKeyword('nameof', 'CFlat extension — compile-time name'),
    makeKeyword('typeof', 'CFlat extension — compile-time type name'),
    makeKeyword('is', 'CFlat extension — runtime type check'),
    makeKeyword('as', 'CFlat extension — safe cast'),
    makeKeyword('foreach', 'CFlat extension — range-based for loop'),
    makeKeyword('move', 'CFlat extension — ownership transfer'),
    makeKeyword('operator', 'CFlat extension — operator overloading'),

    // Core collection types
    makeKeyword('list', 'core library — growable array'),
    makeKeyword('dictionary', 'core library — hash map'),
    makeKeyword('hashset', 'core library — open-addressed set'),
    makeKeyword('stack', 'core library — LIFO'),
    makeKeyword('queue', 'core library — FIFO'),
    makeKeyword('pair', 'core library — two-field struct'),
    makeKeyword('Math', 'core library — math namespace'),
    makeKeyword('stringbuilder', 'core library — mutable string buffer'),

    // Compile-time macros
    { label: '__FILE__',     kind: CompletionItemKind.Constant, detail: 'compile-time macro — current file path' },
    { label: '__FUNCTION__', kind: CompletionItemKind.Constant, detail: 'compile-time macro — current function name' },
    { label: '__LINE__',     kind: CompletionItemKind.Constant, detail: 'compile-time macro — current line number' },
    { label: '__PLATFORM__', kind: CompletionItemKind.Constant, detail: 'compile-time macro — 64 or 32' },

    // C11
    makeKeyword('sizeof', 'operator'),
    makeKeyword('alignof', 'C11 operator'),
    makeKeyword('_Static_assert', 'C11 compile-time assertion'),
    makeKeyword('static_assert', 'C11 compile-time assertion'),
    makeKeyword('_Generic', 'C11 generic selection'),
    makeKeyword('auto', 'type inference'),

    // Constants
    { label: 'true',    kind: CompletionItemKind.Constant, detail: 'boolean constant' },
    { label: 'false',   kind: CompletionItemKind.Constant, detail: 'boolean constant' },
    { label: 'NULL',    kind: CompletionItemKind.Constant, detail: 'null pointer' },
    { label: 'nullptr', kind: CompletionItemKind.Constant, detail: 'null pointer constant' },

    // Snippets
    makeSnippet('if', 'if (${1:condition})\n{\n\t${2}\n}', 'if statement'),
    makeSnippet('if-else', 'if (${1:condition})\n{\n\t${2}\n}\nelse\n{\n\t${3}\n}', 'if-else statement'),
    makeSnippet('while', 'while (${1:condition})\n{\n\t${2}\n}', 'while loop'),
    makeSnippet('for', 'for (int ${1:i} = 0; ${1:i} < ${2:count}; ${1:i}++)\n{\n\t${3}\n}', 'for loop'),
    makeSnippet('foreach', 'foreach (${1:T} ${2:item} in ${3:collection})\n{\n\t${4}\n}', 'foreach loop (CFlat extension)'),
    makeSnippet('switch', 'switch (${1:expr})\n{\n\tcase ${2:value}:\n\t\t${3}\n\t\tbreak;\n\tdefault:\n\t\tbreak;\n}', 'switch statement'),
    makeSnippet('struct', 'struct ${1:Name}\n{\n\t${2}\n};', 'struct declaration'),
    makeSnippet('namespace', 'namespace ${1:Name}\n{\n\t${2}\n}', 'namespace block'),
    makeSnippet('interface', 'interface ${1:Name}\n{\n\t${2}\n};', 'interface declaration'),
    makeSnippet('func', '${1:void} ${2:funcName}(${3})\n{\n\t${4}\n}', 'function definition'),
    makeSnippet('printf', 'printf("${1}\\n"${2});', 'printf call'),
    makeSnippet('#include', '#include <${1:stdio.h}>', '#include directive'),
    makeSnippet('#define', '#define ${1:NAME} ${2:value}', '#define directive'),
    makeSnippet('return-block', 'return {\n\t${1}\n};', 'return block (CFlat extension)'),
    makeSnippet('import', 'import "${1:file.cb}";', 'import declaration (CFlat extension)'),
    makeSnippet('using-alias', 'using ${1:Alias} = ${2:TypeName};', 'type alias (CFlat extension)'),
    makeSnippet('$"', '"${1:text} {${2:expression}}"', 'format string with interpolation'),
    makeSnippet('alignas', 'alignas(${1:alignment})', 'alignas specifier'),
    makeSnippet('static_assert', 'static_assert(${1:condition}, "${2:message}");', 'compile-time assertion')
];
