import { describe, it, expect } from 'vitest';
import {
    removeComments,
    extractStructMembers,
    extractParams,
    indexToPosition,
    getWordAt,
    parseDocumentSymbols,
    parseCompilerOutput,
    CANNOT_BE_TYPE,
    KEYWORD_DOCS,
    BUILTIN_TYPE_SYMBOLS
} from './parser';

// ---------------------------------------------------------------------------
// removeComments
// ---------------------------------------------------------------------------

describe('removeComments', () => {
    it('strips line comments', () => {
        const result = removeComments('int x = 1; // assign x\nint y = 2;');
        expect(result).toContain('int x = 1;');
        expect(result).not.toContain('assign x');
        expect(result).toContain('int y = 2;');
    });

    it('strips block comments', () => {
        const result = removeComments('int /* a comment */ x;');
        expect(result).not.toContain('a comment');
        expect(result).toContain('int');
        expect(result).toContain('x;');
    });

    it('preserves newlines so line numbers stay correct', () => {
        const src = 'a\n// comment\nb';
        const result = removeComments(src);
        expect(result.split('\n')).toHaveLength(3);
    });

    it('handles block comments spanning multiple lines', () => {
        const src = 'a\n/* line1\nline2 */\nb';
        const result = removeComments(src);
        const lines = result.split('\n');
        expect(lines).toHaveLength(4);
        expect(lines[3].trim()).toBe('b');
    });
});

// ---------------------------------------------------------------------------
// extractStructMembers
// ---------------------------------------------------------------------------

describe('extractStructMembers', () => {
    it('extracts simple fields', () => {
        const members = extractStructMembers('    float x = 0;\n    float y = 0;');
        expect(members.map(m => m.name)).toContain('x');
        expect(members.map(m => m.name)).toContain('y');
        expect(members.find(m => m.name === 'x')?.isMethod).toBe(false);
    });

    it('extracts methods', () => {
        const members = extractStructMembers('    int count();\n    void clear();');
        expect(members.find(m => m.name === 'count')?.isMethod).toBe(true);
        expect(members.find(m => m.name === 'clear')?.isMethod).toBe(true);
    });

    it('extracts inline method bodies without duplicating entries', () => {
        const members = extractStructMembers('    int get() { return _val; }');
        const gets = members.filter(m => m.name === 'get');
        expect(gets).toHaveLength(1);
        expect(gets[0].isMethod).toBe(true);
    });

    it('ignores preprocessor directives', () => {
        const members = extractStructMembers('#define FOO 1\n    int x;');
        expect(members.find(m => m.name === 'FOO')).toBeUndefined();
    });

    it('strips field initializers from the type', () => {
        const members = extractStructMembers('    int count = 0;');
        const m = members.find(m => m.name === 'count');
        expect(m).toBeDefined();
        expect(m?.type).toBe('int');
        expect(m?.signature).toBe('int count');
    });
});

// ---------------------------------------------------------------------------
// extractParams
// ---------------------------------------------------------------------------

describe('extractParams', () => {
    it('parses simple parameters', () => {
        const params = extractParams('int x, float y');
        expect(params.map(p => p.name)).toEqual(['x', 'y']);
        expect(params[0].fullType).toBe('int');
        expect(params[1].fullType).toBe('float');
    });

    it('handles pointer parameters', () => {
        const params = extractParams('const char* str');
        expect(params[0].name).toBe('str');
        expect(params[0].fullType).toBe('const char*');
    });

    it('handles generic type parameters without splitting on inner commas', () => {
        const params = extractParams('list<int, string> items, int count');
        expect(params).toHaveLength(2);
        expect(params[0].name).toBe('items');
        expect(params[1].name).toBe('count');
    });

    it('strips default values', () => {
        const params = extractParams('int x = 0, bool flag = true');
        expect(params.map(p => p.name)).toEqual(['x', 'flag']);
    });

    it('ignores void and ellipsis', () => {
        const params = extractParams('void');
        expect(params).toHaveLength(0);
    });

    it('skips move keyword prefix on parameters', () => {
        // "move Resource* r" — 'move' is in CANNOT_BE_TYPE, so full type is "Resource*"
        const params = extractParams('move Resource* r');
        expect(params).toHaveLength(1);
        expect(params[0].name).toBe('r');
    });
});

// ---------------------------------------------------------------------------
// indexToPosition
// ---------------------------------------------------------------------------

describe('indexToPosition', () => {
    it('returns line 0 char 0 for index 0', () => {
        expect(indexToPosition('hello', 0)).toEqual({ line: 0, character: 0 });
    });

    it('returns correct position on first line', () => {
        expect(indexToPosition('hello world', 6)).toEqual({ line: 0, character: 6 });
    });

    it('increments line on newline', () => {
        const text = 'line1\nline2\nline3';
        expect(indexToPosition(text, 6)).toEqual({ line: 1, character: 0 });
        expect(indexToPosition(text, 12)).toEqual({ line: 2, character: 0 });
    });
});

// ---------------------------------------------------------------------------
// getWordAt
// ---------------------------------------------------------------------------

describe('getWordAt', () => {
    it('returns the word at an offset in the middle', () => {
        expect(getWordAt('hello world', 3)).toBe('hello');
    });

    it('returns null when offset is on a space', () => {
        expect(getWordAt('hello world', 5)).toBeNull();
    });

    it('returns the word when offset is at the start', () => {
        expect(getWordAt('foo bar', 0)).toBe('foo');
    });

    it('returns the word when offset is at the last character', () => {
        expect(getWordAt('foo bar', 6)).toBe('bar');
    });
});

// ---------------------------------------------------------------------------
// parseCompilerOutput
// ---------------------------------------------------------------------------

describe('parseCompilerOutput', () => {
    it('parses ANTLR4 format: line N:M message', () => {
        const diags = parseCompilerOutput('line 3:5 no viable alternative at input', 10);
        expect(diags).toHaveLength(1);
        expect(diags[0].range.start.line).toBe(2);
        expect(diags[0].range.start.character).toBe(5);
        expect(diags[0].message).toBe('no viable alternative at input');
        expect(diags[0].source).toBe('cflat');
    });

    it('parses GCC/LLVM error format', () => {
        const diags = parseCompilerOutput('foo.cb:10:4: error: undeclared identifier', 20);
        expect(diags).toHaveLength(1);
        expect(diags[0].range.start.line).toBe(9);
        expect(diags[0].range.start.character).toBe(3);
        expect(diags[0].message).toBe('undeclared identifier');
    });

    it('parses GCC/LLVM warning format', () => {
        const diags = parseCompilerOutput('foo.cb:2:1: warning: unused variable', 10);
        expect(diags[0].severity).toBe(2); // DiagnosticSeverity.Warning === 2
    });

    it('parses generic error with no position', () => {
        const diags = parseCompilerOutput('error: file not found', 5);
        expect(diags).toHaveLength(1);
        expect(diags[0].range.start.line).toBe(0);
        expect(diags[0].message).toBe('file not found');
    });

    it('ignores diagnostics beyond the document line count', () => {
        const diags = parseCompilerOutput('line 99:0 some error', 10);
        expect(diags).toHaveLength(0);
    });

    it('returns empty array for empty output', () => {
        expect(parseCompilerOutput('', 10)).toHaveLength(0);
    });

    it('handles mixed output with multiple diagnostics', () => {
        const output = [
            'line 1:0 syntax error',
            'foo.cb:3:2: warning: unused var',
            'error: fatal issue'
        ].join('\n');
        const diags = parseCompilerOutput(output, 20);
        expect(diags).toHaveLength(3);
    });
});

// ---------------------------------------------------------------------------
// parseDocumentSymbols — structs
// ---------------------------------------------------------------------------

describe('parseDocumentSymbols — structs', () => {
    it('extracts a struct with fields', () => {
        const src = 'struct Point { float x = 0; float y = 0; };';
        const symbols = parseDocumentSymbols(src);
        const pt = symbols.get('Point');
        expect(pt?.kind).toBe('type');
        expect(pt?.members?.map(m => m.name)).toContain('x');
        expect(pt?.members?.map(m => m.name)).toContain('y');
    });

    it('extracts a struct with methods', () => {
        const src = 'struct Foo { int count(); void clear(); };';
        const symbols = parseDocumentSymbols(src);
        const foo = symbols.get('Foo');
        expect(foo?.members?.find(m => m.name === 'count')?.isMethod).toBe(true);
    });

    it('extracts an interface', () => {
        const src = 'interface IDrawable { void draw(); };';
        const symbols = parseDocumentSymbols(src);
        expect(symbols.get('IDrawable')?.kind).toBe('type');
    });
});

// ---------------------------------------------------------------------------
// parseDocumentSymbols — functions
// ---------------------------------------------------------------------------

describe('parseDocumentSymbols — functions', () => {
    it('extracts a top-level function', () => {
        const src = 'int add(int a, int b)\n{\n    return a + b;\n}';
        const symbols = parseDocumentSymbols(src);
        expect(symbols.get('add')?.kind).toBe('function');
    });

    it('extracts function parameters as variable symbols', () => {
        const src = 'void greet(string name, int times) { }';
        const symbols = parseDocumentSymbols(src);
        expect(symbols.get('name')?.kind).toBe('variable');
        expect(symbols.get('times')?.kind).toBe('variable');
    });

    it('records correct defLine for a function', () => {
        const src = 'int first();\nint second();';
        const symbols = parseDocumentSymbols(src);
        expect(symbols.get('first')?.defLine).toBe(0);
        expect(symbols.get('second')?.defLine).toBe(1);
    });
});

// ---------------------------------------------------------------------------
// parseDocumentSymbols — variables
// ---------------------------------------------------------------------------

describe('parseDocumentSymbols — variables', () => {
    it('extracts a simple variable declaration', () => {
        const src = 'int counter = 0;';
        const symbols = parseDocumentSymbols(src);
        expect(symbols.get('counter')?.kind).toBe('variable');
        expect(symbols.get('counter')?.typeName).toBe('int');
    });

    it('extracts a pointer variable', () => {
        const src = 'char* buf = nullptr;';
        const symbols = parseDocumentSymbols(src);
        expect(symbols.get('buf')?.kind).toBe('variable');
    });

    it('extracts for-loop variable', () => {
        // Loop must be on its own line — funcLineRe consumes function-signature lines with continue
        const src = 'void f()\n{\n    for (int i = 0; i < 10; i++) { }\n}';
        const symbols = parseDocumentSymbols(src);
        expect(symbols.get('i')?.kind).toBe('variable');
        expect(symbols.get('i')?.typeName).toBe('int');
    });

    it('extracts foreach-loop variable', () => {
        const src = 'void f(list<int>* nums)\n{\n    foreach (int n in nums) { }\n}';
        const symbols = parseDocumentSymbols(src);
        expect(symbols.get('n')?.kind).toBe('variable');
        expect(symbols.get('n')?.typeName).toBe('int');
    });
});

// ---------------------------------------------------------------------------
// parseDocumentSymbols — namespaces and aliases
// ---------------------------------------------------------------------------

describe('parseDocumentSymbols — namespaces and aliases', () => {
    it('extracts a namespace', () => {
        const src = 'namespace Utils { void helper() { } }';
        const symbols = parseDocumentSymbols(src);
        expect(symbols.get('Utils')?.kind).toBe('namespace');
    });

    it('extracts a type alias', () => {
        const src = 'using Vec = Math.Vector2;';
        const symbols = parseDocumentSymbols(src);
        expect(symbols.get('Vec')?.kind).toBe('type');
        expect(symbols.get('Vec')?.typeName).toBe('Vector2');
    });

    it('extracts a #define macro', () => {
        const src = '#define MAX_SIZE 256';
        const symbols = parseDocumentSymbols(src);
        expect(symbols.get('MAX_SIZE')?.kind).toBe('macro');
    });
});

// ---------------------------------------------------------------------------
// parseDocumentSymbols — built-in types injected
// ---------------------------------------------------------------------------

describe('parseDocumentSymbols — built-in types', () => {
    it('always provides string symbol', () => {
        const symbols = parseDocumentSymbols('');
        expect(symbols.get('string')?.kind).toBe('type');
        expect(symbols.get('string')?.members?.map(m => m.name)).toContain('length');
    });

    it('always provides list symbol', () => {
        const symbols = parseDocumentSymbols('');
        expect(symbols.get('list')?.kind).toBe('type');
        expect(symbols.get('list')?.members?.map(m => m.name)).toContain('add');
        expect(symbols.get('list')?.members?.map(m => m.name)).toContain('count');
    });

    it('always provides Math symbol', () => {
        const symbols = parseDocumentSymbols('');
        expect(symbols.get('Math')?.kind).toBe('namespace');
        expect(symbols.get('Math')?.members?.map(m => m.name)).toContain('sqrt');
    });
});

// ---------------------------------------------------------------------------
// CANNOT_BE_TYPE — new keywords
// ---------------------------------------------------------------------------

describe('CANNOT_BE_TYPE', () => {
    it('includes foreach', () => expect(CANNOT_BE_TYPE.has('foreach')).toBe(true));
    it('includes move',    () => expect(CANNOT_BE_TYPE.has('move')).toBe(true));
    it('includes operator', () => expect(CANNOT_BE_TYPE.has('operator')).toBe(true));
});

// ---------------------------------------------------------------------------
// KEYWORD_DOCS — new entries present
// ---------------------------------------------------------------------------

describe('KEYWORD_DOCS', () => {
    it('has foreach documentation', () => expect(KEYWORD_DOCS['foreach']).toBeTruthy());
    it('has move documentation',    () => expect(KEYWORD_DOCS['move']).toBeTruthy());
    it('has operator documentation', () => expect(KEYWORD_DOCS['operator']).toBeTruthy());
    it('has __PLATFORM__ documentation', () => expect(KEYWORD_DOCS['__PLATFORM__']).toBeTruthy());
    it('has compile-time macros', () => {
        expect(KEYWORD_DOCS['__FILE__']).toBeTruthy();
        expect(KEYWORD_DOCS['__FUNCTION__']).toBeTruthy();
        expect(KEYWORD_DOCS['__LINE__']).toBeTruthy();
    });
});

// ---------------------------------------------------------------------------
// BUILTIN_TYPE_SYMBOLS — collection types
// ---------------------------------------------------------------------------

describe('BUILTIN_TYPE_SYMBOLS', () => {
    it('includes all core collection types', () => {
        for (const name of ['list', 'dictionary', 'hashset', 'stack', 'queue', 'pair']) {
            expect(BUILTIN_TYPE_SYMBOLS.has(name)).toBe(true);
        }
    });

    it('stack has push/pop/peek/count members', () => {
        const members = BUILTIN_TYPE_SYMBOLS.get('stack')?.members?.map(m => m.name);
        expect(members).toContain('push');
        expect(members).toContain('pop');
        expect(members).toContain('peek');
        expect(members).toContain('count');
    });

    it('dictionary has add/get/remove/count members', () => {
        const members = BUILTIN_TYPE_SYMBOLS.get('dictionary')?.members?.map(m => m.name);
        expect(members).toContain('add');
        expect(members).toContain('get');
        expect(members).toContain('remove');
        expect(members).toContain('count');
    });
});
