import {
    createConnection,
    TextDocuments,
    Diagnostic,
    DiagnosticSeverity,
    ProposedFeatures,
    InitializeParams,
    DidChangeConfigurationNotification,
    CompletionItem,
    CompletionItemKind,
    TextDocumentPositionParams,
    TextDocumentSyncKind,
    InitializeResult,
    Hover,
    MarkupKind,
    InsertTextFormat,
    Location
} from 'vscode-languageserver/node';
import { TextDocument } from 'vscode-languageserver-textdocument';
import * as cp from 'child_process';
import * as fs from 'fs';
import * as path from 'path';
import * as os from 'os';

const connection = createConnection(ProposedFeatures.all);
const documents = new TextDocuments<TextDocument>(TextDocument);

let hasConfigurationCapability = false;
let workspaceFolders: string[] = [];

// ---------------------------------------------------------------------------
// Settings
// ---------------------------------------------------------------------------

interface MycSettings {
    executablePath: string;
    enableDiagnostics: boolean;
    diagnosticDelay: number;
}

const defaultSettings: MycSettings = {
    executablePath: '',
    enableDiagnostics: true,
    diagnosticDelay: 500
};

let globalSettings: MycSettings = defaultSettings;
const documentSettings = new Map<string, Promise<MycSettings>>();
const diagnosticTimers = new Map<string, ReturnType<typeof setTimeout>>();

// ---------------------------------------------------------------------------
// Symbol table (user-defined symbols for hover / completion enrichment)
// ---------------------------------------------------------------------------

interface MemberInfo {
    name: string;
    type: string;
    isMethod: boolean;
    /** Display signature, e.g. "int count" or "void foo(int x)". */
    signature: string;
}

interface SymbolInfo {
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

/** Per-document symbol cache, invalidated on every content change. */
const documentSymbols = new Map<string, Map<string, SymbolInfo>>();

// ---------------------------------------------------------------------------
// Initialization
// ---------------------------------------------------------------------------

connection.onInitialize((params: InitializeParams): InitializeResult => {
    const capabilities = params.capabilities;
    hasConfigurationCapability = !!(
        capabilities.workspace && !!capabilities.workspace.configuration
    );

    if (params.workspaceFolders) {
        workspaceFolders = params.workspaceFolders.map(f => uriToFilePath(f.uri));
    }

    return {
        capabilities: {
            textDocumentSync: TextDocumentSyncKind.Incremental,
            completionProvider: {
                resolveProvider: true,
                triggerCharacters: ['.', '>', ':', '#', '<']
            },
            hoverProvider: true,
            definitionProvider: true
        }
    };
});

connection.onInitialized(() => {
    if (hasConfigurationCapability) {
        connection.client.register(DidChangeConfigurationNotification.type, undefined);
    }
});

// ---------------------------------------------------------------------------
// Configuration
// ---------------------------------------------------------------------------

connection.onDidChangeConfiguration(() => {
    if (hasConfigurationCapability) {
        documentSettings.clear();
    } else {
        globalSettings = defaultSettings;
    }
    documents.all().forEach(validateDocument);
});

function getDocumentSettings(resource: string): Promise<MycSettings> {
    if (!hasConfigurationCapability) {
        return Promise.resolve(globalSettings);
    }
    let result = documentSettings.get(resource);
    if (!result) {
        result = connection.workspace.getConfiguration({
            scopeUri: resource,
            section: 'mycompiler'
        }) as Promise<MycSettings>;
        documentSettings.set(resource, result);
    }
    return result;
}

// ---------------------------------------------------------------------------
// Document lifecycle
// ---------------------------------------------------------------------------

documents.onDidClose(e => {
    documentSettings.delete(e.document.uri);
    documentSymbols.delete(e.document.uri);
    connection.sendDiagnostics({ uri: e.document.uri, diagnostics: [] });
    const timer = diagnosticTimers.get(e.document.uri);
    if (timer) {
        clearTimeout(timer);
        diagnosticTimers.delete(e.document.uri);
    }
});

documents.onDidChangeContent(change => {
    documentSymbols.delete(change.document.uri);
    scheduleDiagnostics(change.document);
});

documents.onDidSave(e => {
    validateDocument(e.document);
});

function scheduleDiagnostics(document: TextDocument): void {
    const existing = diagnosticTimers.get(document.uri);
    if (existing) clearTimeout(existing);

    getDocumentSettings(document.uri).then(settings => {
        const timer = setTimeout(() => {
            diagnosticTimers.delete(document.uri);
            validateDocument(document);
        }, settings.diagnosticDelay);
        diagnosticTimers.set(document.uri, timer);
    });
}

// ---------------------------------------------------------------------------
// Compiler executable resolution
// ---------------------------------------------------------------------------

function findCompilerExecutable(settings: MycSettings, workspaceFolders: string[]): string | null {
    if (settings.executablePath && settings.executablePath.trim() !== '') {
        return settings.executablePath.trim();
    }

    // Search common build output locations relative to workspace roots
    const candidates = [
        'x64\\Debug\\MyCompiler.exe',
        'x64\\Release\\MyCompiler.exe',
        'Win32\\Debug\\MyCompiler.exe',
        'Win32\\Release\\MyCompiler.exe',
        'Debug\\MyCompiler.exe',
        'Release\\MyCompiler.exe',
        'build\\MyCompiler.exe',
        'MyCompiler.exe'
    ];

    for (const folder of workspaceFolders) {
        for (const candidate of candidates) {
            const full = path.join(folder, candidate);
            if (fs.existsSync(full)) {
                connection.console.log(`Found MyCompiler.exe at: ${full}`);
                return full;
            }
        }
    }

    return null;
}

// ---------------------------------------------------------------------------
// Diagnostics
// ---------------------------------------------------------------------------

async function validateDocument(document: TextDocument): Promise<void> {
    const settings = await getDocumentSettings(document.uri);

    if (!settings.enableDiagnostics) {
        connection.sendDiagnostics({ uri: document.uri, diagnostics: [] });
        return;
    }

    const exePath = findCompilerExecutable(settings, workspaceFolders);
    if (!exePath) {
        connection.console.warn(
            'MyCompiler.exe not found. Set mycompiler.executablePath in settings, ' +
            'or build the project so the executable appears in a standard output directory.'
        );
        connection.sendDiagnostics({ uri: document.uri, diagnostics: [] });
        return;
    }

    // Write the current document content to a temp file so unsaved changes are diagnosed too
    const ext = path.extname(uriToFilePath(document.uri)) || '.myc';
    const tmpFile = path.join(os.tmpdir(), `myc_diag_${Date.now()}${ext}`);

    try {
        fs.writeFileSync(tmpFile, document.getText(), 'utf-8');
    } catch (e) {
        connection.console.error(`Failed to write temp file: ${e}`);
        return;
    }

    const diagnostics = await runCompiler(exePath, tmpFile, document);

    try { fs.unlinkSync(tmpFile); } catch { /* ignore */ }

    connection.sendDiagnostics({ uri: document.uri, diagnostics });
}

function runCompiler(
    exePath: string,
    inputFile: string,
    document: TextDocument
): Promise<Diagnostic[]> {
    return new Promise(resolve => {
        const args = [inputFile, '-o', os.devNull];
        connection.console.log(`Running: "${exePath}" ${args.join(' ')}`);

        const proc = cp.spawn(exePath, args, { stdio: ['ignore', 'pipe', 'pipe'] });

        let stdout = '';
        let stderr = '';
        proc.stdout?.on('data', (d: Buffer) => { stdout += d.toString(); });
        proc.stderr?.on('data', (d: Buffer) => { stderr += d.toString(); });

        proc.on('error', err => {
            connection.console.error(`Failed to start MyCompiler.exe: ${err.message}`);
            resolve([]);
        });

        proc.on('close', () => {
            const output = stderr + '\n' + stdout;
            const diagnostics = parseCompilerOutput(output, document);
            resolve(diagnostics);
        });

        // Kill if it takes too long
        setTimeout(() => {
            proc.kill();
        }, 10000);
    });
}

/**
 * Parse compiler diagnostic output.
 *
 * Handles these formats:
 *   ANTLR4:   line 5:10 no viable alternative at input 'foo'
 *   LLVM/GCC: filename:5:10: error: message
 *             filename:5:10: warning: message
 *   Generic:  error: message  (no position)
 */
function parseCompilerOutput(output: string, document: TextDocument): Diagnostic[] {
    const diagnostics: Diagnostic[] = [];
    const lineCount = document.lineCount;

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
                    source: 'MyCompiler'
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
                    source: 'MyCompiler'
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
                source: 'MyCompiler'
            });
        }
    }

    return diagnostics;
}

// ---------------------------------------------------------------------------
// Manual trigger notification
// ---------------------------------------------------------------------------

connection.onNotification('mycompiler/runDiagnostics', (params: { uri: string }) => {
    const doc = documents.get(params.uri);
    if (doc) validateDocument(doc);
});

// ---------------------------------------------------------------------------
// Hover
// ---------------------------------------------------------------------------

connection.onHover((params: TextDocumentPositionParams): Hover | null => {
    const document = documents.get(params.textDocument.uri);
    if (!document) return null;

    const word = getWordAtPosition(document, params.position);
    if (!word) return null;

    // User-defined symbols take priority over keyword docs
    const symbols = getSymbolsForDocument(document);
    const symInfo = symbols.get(word);
    if (symInfo) {
        return { contents: { kind: MarkupKind.Markdown, value: symInfo.markdown } };
    }

    const doc = KEYWORD_DOCS[word];
    if (!doc) return null;

    return {
        contents: {
            kind: MarkupKind.Markdown,
            value: doc
        }
    };
});

// ---------------------------------------------------------------------------
// Go to Definition (F12)
// ---------------------------------------------------------------------------

connection.onDefinition((params: TextDocumentPositionParams): Location | null => {
    const document = documents.get(params.textDocument.uri);
    if (!document) return null;

    const word = getWordAtPosition(document, params.position);
    if (!word) return null;

    const symbols = getSymbolsForDocument(document);
    const symInfo = symbols.get(word);
    if (!symInfo) return null;

    return {
        uri: params.textDocument.uri,
        range: {
            start: { line: symInfo.defLine, character: symInfo.defChar },
            end:   { line: symInfo.defLine, character: symInfo.defChar + word.length }
        }
    };
});

function getWordAtPosition(document: TextDocument, position: { line: number; character: number }): string | null {
    const text = document.getText();
    const offset = document.offsetAt(position);

    let start = offset;
    let end = offset;

    while (start > 0 && /\w/.test(text[start - 1])) start--;
    while (end < text.length && /\w/.test(text[end])) end++;

    return start === end ? null : text.slice(start, end);
}

// ---------------------------------------------------------------------------
// Symbol extraction
// ---------------------------------------------------------------------------

function getSymbolsForDocument(document: TextDocument): Map<string, SymbolInfo> {
    let symbols = documentSymbols.get(document.uri);
    if (!symbols) {
        symbols = parseDocumentSymbols(document.getText());
        documentSymbols.set(document.uri, symbols);
    }
    return symbols;
}

/**
 * Strip line and block comments, preserving newlines so line numbers stay intact.
 */
function removeComments(text: string): string {
    return text
        .replace(/\/\*[\s\S]*?\*\//g, m => m.replace(/[^\n]/g, ' '))
        .replace(/\/\/[^\n]*/g, m => ' '.repeat(m.length));
}

/**
 * Extract members (fields + methods) from a struct/class body string.
 * The body is the text between the outer `{` and `}` of the aggregate.
 */
function extractStructMembers(body: string): MemberInfo[] {
    const members: MemberInfo[] = [];
    let depth = 0;
    let segStart = 0;

    const addMember = (segment: string) => {
        const norm = segment.replace(/\s+/g, ' ').trim().replace(/[;{}]$/, '').trim();
        if (!norm || norm.startsWith('#') || norm.length > 150) return;

        if (norm.includes('(')) {
            // Method — extract name and return type from signature before '('
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
            // Field — strip initializer (= ...) then extract type and name
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
                // Inline method body just closed — capture its signature
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

/** Convert a character index in `text` to a 0-based { line, character } position. */
function indexToPosition(text: string, index: number): { line: number; character: number } {
    const before = text.slice(0, index);
    const lines = before.split('\n');
    return { line: lines.length - 1, character: lines[lines.length - 1].length };
}

/** Words that cannot be C type names (control-flow keywords, etc.). */
const CANNOT_BE_TYPE = new Set([
    'if', 'else', 'while', 'for', 'do', 'switch', 'case', 'default',
    'break', 'continue', 'return', 'goto', 'using', 'namespace', 'interface',
    'sizeof', 'typeof', 'nameof', 'alignof'
]);

/**
 * Scan a document and produce a symbol table for user-defined identifiers.
 * Handles: #define macros, namespaces, structs/classes/unions/enums (with
 * member lists), interfaces, function declarations, and variable declarations.
 */
function parseDocumentSymbols(text: string): Map<string, SymbolInfo> {
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
    // No brace required: handles both K&R "namespace Foo {" and Allman "namespace Foo\n{"
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
    const aggRe = /\b(struct|class|union|enum)\s+([A-Za-z_]\w*)\s*\{/g;
    while ((m = aggRe.exec(clean)) !== null) {
        const nameIndex = m.index + m[0].indexOf(m[2]);
        extractAggregate(m[1], m[2], m.index + m[0].length, nameIndex);
    }

    // --- interface (MyC extension) ---
    const ifaceRe = /\binterface\s+([A-Za-z_]\w*)\s*\{/g;
    while ((m = ifaceRe.exec(clean)) !== null) {
        const nameIndex = m.index + m[0].indexOf(m[1]);
        extractAggregate('interface', m[1], m.index + m[0].length, nameIndex);
    }

    // --- Function and variable declarations (line-by-line) ---
    //
    // Function: [qualifiers] returnType[*] qualName(params) [const] [{;]
    //   qualName allows Namespace::funcName for out-of-namespace definitions.
    //   [{;] is optional to handle Allman-style braces (brace on next line).
    //
    // Variable: [qualifiers] type[*] name [array] [= ...];
    //   Pointer separator uses (\s*\*+\s*|\s+) so both "int *p" and "int* p" work.

    // No trailing $ on funcLineRe: dropping it lets single-line bodies
    // "int f() { return 0; }" match, and avoids failure when removeComments
    // leaves trailing spaces after the opening brace.
    const funcLineRe = /^([ \t]*(?:(?:static|extern|inline|virtual)\s+)*)((?:const\s+)?(?:(?:unsigned|signed|long|short)\s+)*(?:(?:struct|class|union|enum)\s+)?[A-Za-z_]\w*(?:\s*\*+)?)\s+([A-Za-z_][\w:]*)\s*\(([^)]*)\)(?:\s*const)?\s*[{;]?/;
    const varLineRe   = /^([ \t]*(?:(?:const|volatile|static|extern|register)\s+)*)((?:(?:unsigned|signed|long|short)\s+)*(?:(?:struct|class|union|enum)\s+)?[A-Za-z_]\w*)(\s*\*+\s*|\s+)([A-Za-z_]\w*)\s*(?:\[.*?\])?\s*(?:=.*)?\s*;$/;
    // For-loop variable initializer: for (type name = ...)
    const forVarRe    = /\bfor\s*\(\s*((?:(?:const|volatile)\s+)?(?:(?:unsigned|signed|long|short)\s+)*(?:(?:struct|class|union|enum)\s+)?[A-Za-z_]\w*)(\s*\*+\s*|\s+)([A-Za-z_]\w*)\s*=/g;

    const addVarSymbol = (
        name: string, fullType: string,
        lineIdx: number, defChar: number
    ) => {
        if (!symbols.has(name) && !KEYWORD_DOCS[name]) {
            const typeName = fullType.replace(/\*+$/, '').trim();
            symbols.set(name, {
                kind: 'variable',
                markdown: `\`\`\`c\n${fullType} ${name}\n\`\`\``,
                defLine: lineIdx,
                defChar: Math.max(0, defChar),
                typeName
            });
        }
    };

    // trimEnd() removes trailing spaces left by removeComments (comment text
    // is replaced with spaces, which would break $ anchors in varLineRe).
    const lines = clean.split('\n');
    for (let lineIdx = 0; lineIdx < lines.length; lineIdx++) {
        const rawLine = lines[lineIdx].trimEnd();

        // --- Function pattern ---
        const fm = funcLineRe.exec(rawLine);
        if (fm) {
            const retType = fm[2].trim();
            const qualName = fm[3];           // may be "Namespace::funcName"
            const shortName = qualName.split('::').pop()!;
            const params = fm[4].trim();

            if (!KEYWORD_DOCS[shortName] && !CANNOT_BE_TYPE.has(retType.split(/\s+/)[0])) {
                const parenIdx = rawLine.indexOf('(');
                // Column of the short name = last word before '('
                const beforeParen = parenIdx > 0 ? rawLine.slice(0, parenIdx) : '';
                const defChar = Math.max(0, beforeParen.lastIndexOf(shortName));

                const info: SymbolInfo = {
                    kind: 'function',
                    markdown: `\`\`\`c\n${retType} ${shortName}(${params})\n\`\`\``,
                    defLine: lineIdx,
                    defChar
                };
                // Register under the short name (for callsite lookup) and qualified name
                if (!symbols.has(shortName)) symbols.set(shortName, info);
                if (qualName !== shortName && !symbols.has(qualName)) symbols.set(qualName, info);
            }
            continue;
        }

        // --- Variable pattern (handles int *p and int* p) ---
        const vm = varLineRe.exec(rawLine);
        if (vm) {
            const baseType = vm[2].trim();
            const sep      = vm[3];               // spacing/stars between type and name
            const name     = vm[4];
            const ptrStars = sep.trim();           // just the '*' characters
            const fullType = ptrStars ? `${baseType}${ptrStars}` : baseType;

            if (!CANNOT_BE_TYPE.has(baseType.split(/\s+/)[0])) {
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
    }

    return symbols;
}

// ---------------------------------------------------------------------------
// Completions
// ---------------------------------------------------------------------------

connection.onCompletion((params: TextDocumentPositionParams): CompletionItem[] => {
    const document = documents.get(params.textDocument.uri);
    if (!document) return COMPLETION_ITEMS;

    const textBefore = document.getText().slice(0, document.offsetAt(params.position));

    // Detect member access: <identifier> . <partial>  or  <identifier> -> <partial>
    const memberMatch = /(\w+)\s*(?:->|\.)\w*$/.exec(textBefore);
    if (memberMatch) {
        const varName = memberMatch[1];
        const symbols = getSymbolsForDocument(document);
        const varInfo = symbols.get(varName);
        const typeName = varInfo?.typeName ?? (varInfo?.kind === 'type' ? varName : undefined);
        if (typeName) {
            const typeInfo = symbols.get(typeName);
            if (typeInfo?.members && typeInfo.members.length > 0) {
                return typeInfo.members.map(mb => ({
                    label: mb.name,
                    kind: mb.isMethod ? CompletionItemKind.Method : CompletionItemKind.Field,
                    detail: mb.signature
                }));
            }
        }
        return [];  // member access context but type unknown — show nothing
    }

    return COMPLETION_ITEMS;
});

connection.onCompletionResolve((item: CompletionItem): CompletionItem => {
    const detail = KEYWORD_DOCS[item.label as string];
    if (detail) {
        item.documentation = { kind: MarkupKind.Markdown, value: detail };
    }
    return item;
});

// ---------------------------------------------------------------------------
// Keyword documentation
// ---------------------------------------------------------------------------

const KEYWORD_DOCS: Record<string, string> = {
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

    // MyC extensions
    'namespace': '**namespace** *(MyC extension)*\n\nGroup declarations under a named scope.\n\n```c\nnamespace Math {\n    float pi = 3.14159f;\n    float sqrt(float x) { ... }\n}\n```',
    'using':     '**using** *(MyC extension)*\n\nImport a namespace into the current scope.\n\n```c\nusing Math;\nusing Math::sqrt;\n```',
    'interface': '**interface** *(MyC extension)*\n\nDeclare an abstract set of method signatures that a struct must implement.\n\n```c\ninterface Drawable {\n    void draw();\n};\n```',
    'nameof':    '**nameof** *(MyC extension)*\n\nReturn the name of a variable or type as a `const char*` string at compile time.\n\n```c\nconst char* name = nameof(myVariable);\n```',
    'typeof':    '**typeof** *(MyC extension)*\n\nReturn the type name of an expression as a `const char*` string.\n\n```c\nconst char* typeName = typeof(myVar);\n```',

    // C11
    'sizeof':         '**sizeof** *(operator)*\n\nReturn the size in bytes of a type or expression.',
    'alignof':        '**alignof** *(C11)*\n\nReturn the alignment requirement of a type.',
    '_Static_assert': '**_Static_assert** *(C11)*\n\nCompile-time assertion. `_Static_assert(sizeof(int) == 4, "int must be 4 bytes");`',

    // Constants
    'true':    '**true** *(constant)*\n\nBoolean true (1).',
    'false':   '**false** *(constant)*\n\nBoolean false (0).',
    'NULL':    '**NULL** *(constant)*\n\nNull pointer constant.',
    'nullptr': '**nullptr** *(constant)*\n\nNull pointer constant (C++ style alias).'
};

// ---------------------------------------------------------------------------
// Completion items
// ---------------------------------------------------------------------------

function makeKeyword(label: string, detail?: string): CompletionItem {
    return { label, kind: CompletionItemKind.Keyword, detail: detail ?? 'keyword' };
}

function makeSnippet(label: string, insertText: string, detail: string): CompletionItem {
    return {
        label,
        kind: CompletionItemKind.Snippet,
        insertText,
        insertTextFormat: InsertTextFormat.Snippet,
        detail
    };
}

const COMPLETION_ITEMS: CompletionItem[] = [
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

    // MyC extensions
    makeKeyword('namespace', 'MyC extension'),
    makeKeyword('using', 'MyC extension'),
    makeKeyword('interface', 'MyC extension'),
    makeKeyword('nameof', 'MyC extension — compile-time name'),
    makeKeyword('typeof', 'MyC extension — compile-time type name'),

    // C11
    makeKeyword('sizeof', 'operator'),
    makeKeyword('alignof', 'C11 operator'),
    makeKeyword('_Static_assert', 'C11 compile-time assertion'),
    makeKeyword('_Generic', 'C11 generic selection'),
    makeKeyword('auto', 'storage class'),

    // Constants
    { label: 'true', kind: CompletionItemKind.Constant, detail: 'boolean constant' },
    { label: 'false', kind: CompletionItemKind.Constant, detail: 'boolean constant' },
    { label: 'NULL', kind: CompletionItemKind.Constant, detail: 'null pointer' },

    // Snippets
    makeSnippet('if', 'if (${1:condition})\n{\n\t${2}\n}', 'if statement'),
    makeSnippet('if-else', 'if (${1:condition})\n{\n\t${2}\n}\nelse\n{\n\t${3}\n}', 'if-else statement'),
    makeSnippet('while', 'while (${1:condition})\n{\n\t${2}\n}', 'while loop'),
    makeSnippet('for', 'for (int ${1:i} = 0; ${1:i} < ${2:count}; ${1:i}++)\n{\n\t${3}\n}', 'for loop'),
    makeSnippet('switch', 'switch (${1:expr})\n{\n\tcase ${2:value}:\n\t\t${3}\n\t\tbreak;\n\tdefault:\n\t\tbreak;\n}', 'switch statement'),
    makeSnippet('struct', 'struct ${1:Name}\n{\n\t${2}\n};', 'struct declaration'),
    makeSnippet('namespace', 'namespace ${1:Name}\n{\n\t${2}\n}', 'namespace block'),
    makeSnippet('interface', 'interface ${1:Name}\n{\n\t${2}\n};', 'interface declaration'),
    makeSnippet('func', '${1:void} ${2:funcName}(${3})\n{\n\t${4}\n}', 'function definition'),
    makeSnippet('printf', 'printf("${1}\\n"${2});', 'printf call'),
    makeSnippet('#include', '#include <${1:stdio.h}>', '#include directive'),
    makeSnippet('#define', '#define ${1:NAME} ${2:value}', '#define directive'),
    makeSnippet('return-block', 'return {\n\t${1}\n};', 'return block (MyC extension)')
];

// ---------------------------------------------------------------------------
// Utilities
// ---------------------------------------------------------------------------

function uriToFilePath(uri: string): string {
    // Handle file:///C:/... style URIs on Windows
    if (uri.startsWith('file:///')) {
        const decoded = decodeURIComponent(uri.slice('file:///'.length));
        return decoded.replace(/\//g, path.sep);
    }
    if (uri.startsWith('file://')) {
        return decodeURIComponent(uri.slice('file://'.length));
    }
    return uri;
}

// ---------------------------------------------------------------------------
// Start
// ---------------------------------------------------------------------------

documents.listen(connection);
connection.listen();
