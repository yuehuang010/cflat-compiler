// DEPRECATED: replaced by cflat lsp (stdio transport in extension.ts).
// Remove once the native LSP server is stable.
import {
    createConnection,
    TextDocuments,
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
    Location
} from 'vscode-languageserver/node';
import { TextDocument } from 'vscode-languageserver-textdocument';
import * as cp from 'child_process';
import * as fs from 'fs';
import * as path from 'path';
import * as os from 'os';

import {
    SymbolInfo,
    KEYWORD_DOCS,
    COMPLETION_ITEMS,
    parseDocumentSymbols,
    parseCompilerOutput,
    getWordAt
} from './parser';

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
                triggerCharacters: ['.', '>', ':', '#', '<', '?']
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
            section: 'cflat'
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

function findCompilerExecutable(settings: MycSettings, folders: string[]): string | null {
    if (settings.executablePath && settings.executablePath.trim() !== '') {
        return settings.executablePath.trim();
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
            'cflat.executablePath is not set. Set it in Settings to enable diagnostics.'
        );
        connection.sendDiagnostics({ uri: document.uri, diagnostics: [] });
        return;
    }

    const ext = path.extname(uriToFilePath(document.uri)) || '.cb';
    const tmpFile = path.join(os.tmpdir(), `cflat_diag_${Date.now()}${ext}`);

    try {
        fs.writeFileSync(tmpFile, document.getText(), 'utf-8');
    } catch (e) {
        connection.console.error(`Failed to write temp file: ${e}`);
        return;
    }

    const diagnostics = await runCompiler(exePath, tmpFile, document.lineCount);

    try { fs.unlinkSync(tmpFile); } catch { /* ignore */ }

    connection.sendDiagnostics({ uri: document.uri, diagnostics });
}

function runCompiler(exePath: string, inputFile: string, lineCount: number) {
    return new Promise<ReturnType<typeof parseCompilerOutput>>(resolve => {
        const outputFile = path.join(os.tmpdir(), `cflat_out_${Date.now()}.tmp`);
        const args = [inputFile, '-o', outputFile];
        connection.console.log(`Running: "${exePath}" ${args.join(' ')}`);

        const proc = cp.spawn(exePath, args, { stdio: ['ignore', 'pipe', 'pipe'] });

        let stdout = '';
        let stderr = '';
        proc.stdout?.on('data', (d: Buffer) => { stdout += d.toString(); });
        proc.stderr?.on('data', (d: Buffer) => { stderr += d.toString(); });

        proc.on('error', err => {
            connection.console.error(`Failed to start cflat: ${err.message}`);
            resolve([]);
        });

        proc.on('close', () => {
            const output = stderr + '\n' + stdout;
            const diagnostics = parseCompilerOutput(output, lineCount);
            try { fs.unlinkSync(outputFile); } catch { /* ignore */ }
            resolve(diagnostics);
        });

        setTimeout(() => { proc.kill(); }, 10000);
    });
}

// ---------------------------------------------------------------------------
// Manual trigger notification
// ---------------------------------------------------------------------------

connection.onNotification('cflat/runDiagnostics', (params: { uri: string }) => {
    const doc = documents.get(params.uri);
    if (doc) validateDocument(doc);
});

// ---------------------------------------------------------------------------
// Symbol cache
// ---------------------------------------------------------------------------

/** Per-document symbol cache, invalidated on every content change. */
const documentSymbols = new Map<string, Map<string, SymbolInfo>>();

function getSymbolsForDocument(document: TextDocument): Map<string, SymbolInfo> {
    let symbols = documentSymbols.get(document.uri);
    if (!symbols) {
        symbols = parseDocumentSymbols(document.getText());
        documentSymbols.set(document.uri, symbols);
    }
    return symbols;
}

// ---------------------------------------------------------------------------
// Hover
// ---------------------------------------------------------------------------

connection.onHover((params: TextDocumentPositionParams): Hover | null => {
    const document = documents.get(params.textDocument.uri);
    if (!document) return null;

    const text = document.getText();
    const offset = document.offsetAt(params.position);
    const word = getWordAt(text, offset);
    if (!word) return null;

    const symbols = getSymbolsForDocument(document);
    const symInfo = symbols.get(word);
    if (symInfo) {
        return { contents: { kind: MarkupKind.Markdown, value: symInfo.markdown } };
    }

    const doc = KEYWORD_DOCS[word];
    if (!doc) return null;

    return { contents: { kind: MarkupKind.Markdown, value: doc } };
});

// ---------------------------------------------------------------------------
// Go to Definition (F12)
// ---------------------------------------------------------------------------

connection.onDefinition((params: TextDocumentPositionParams): Location | null => {
    const document = documents.get(params.textDocument.uri);
    if (!document) return null;

    const text = document.getText();
    const offset = document.offsetAt(params.position);
    const word = getWordAt(text, offset);
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

// ---------------------------------------------------------------------------
// Completions
// ---------------------------------------------------------------------------

connection.onCompletion((params: TextDocumentPositionParams): CompletionItem[] => {
    const document = documents.get(params.textDocument.uri);
    if (!document) return COMPLETION_ITEMS;

    const textBefore = document.getText().slice(0, document.offsetAt(params.position));

    // Detect member access: obj.member  or  obj->member  or  obj?.member (null-conditional)
    const memberMatch = /(\w+)\s*(?:->|\??\.)\w*$/.exec(textBefore);
    if (memberMatch) {
        const varName = memberMatch[1];
        const symbols = getSymbolsForDocument(document);
        const varInfo = symbols.get(varName);

        if (varInfo?.kind === 'variable' && varInfo.members && varInfo.members.length > 0) {
            return varInfo.members.map(mb => ({
                label: mb.name,
                kind: mb.isMethod ? CompletionItemKind.Method : CompletionItemKind.Field,
                detail: mb.signature
            }));
        }

        const rawTypeName = varInfo?.typeName ?? (varInfo?.kind === 'type' ? varName : undefined);
        const typeName = rawTypeName?.replace(/<[^>]*>/g, '').trim();
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
        return [];
    }

    const docSymbols = getSymbolsForDocument(document);
    const userItems: CompletionItem[] = [];
    const seenMembers = new Set<string>();

    for (const [name, info] of docSymbols) {
        let kind: CompletionItemKind;
        switch (info.kind) {
            case 'function':  kind = CompletionItemKind.Function; break;
            case 'type':      kind = CompletionItemKind.Struct;   break;
            case 'variable':  kind = CompletionItemKind.Variable; break;
            case 'macro':     kind = CompletionItemKind.Constant; break;
            case 'namespace': kind = CompletionItemKind.Module;   break;
            default:          kind = CompletionItemKind.Text;     break;
        }
        userItems.push({
            label: name,
            kind,
            detail: info.kind,
            documentation: { kind: MarkupKind.Markdown, value: info.markdown }
        });

        if (info.kind === 'type' && info.members) {
            for (const mb of info.members) {
                if (seenMembers.has(mb.name)) continue;
                seenMembers.add(mb.name);
                userItems.push({
                    label: mb.name,
                    kind: mb.isMethod ? CompletionItemKind.Method : CompletionItemKind.Field,
                    detail: `${mb.isMethod ? 'method' : 'field'} of ${name}`,
                    documentation: { kind: MarkupKind.Markdown, value: `\`\`\`c\n${mb.signature};\n\`\`\`` }
                });
            }
        }
    }

    return [...userItems, ...COMPLETION_ITEMS];
});

connection.onCompletionResolve((item: CompletionItem): CompletionItem => {
    const detail = KEYWORD_DOCS[item.label as string];
    if (detail) {
        item.documentation = { kind: MarkupKind.Markdown, value: detail };
    }
    return item;
});

// ---------------------------------------------------------------------------
// Utilities
// ---------------------------------------------------------------------------

function uriToFilePath(uri: string): string {
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
