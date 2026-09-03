import * as childProcess from 'child_process';
import * as fs from 'fs';
import * as path from 'path';
import * as vscode from 'vscode';
import * as l10n from '@vscode/l10n';
import {
    OptFunctionInfo, OptInfoCacheEntry, OptInfoResult,
    buildCallSiteHover, buildFunctionDetail, collectInlineCallSites,
    countCallSites, countFunctions, countInstructions, countReloads, countSpills
} from './optimization_info';
import {
    LanguageClient,
    LanguageClientOptions,
    ServerOptions,
    TransportKind
} from 'vscode-languageclient/node';

let client: LanguageClient | undefined;
let outputChannel: vscode.LogOutputChannel;
let logFilePath: string;

type ViewKind = 'ir' | 'asm';

interface ViewChoice {
    id: string;
    label: string;
    optimized: boolean;
    wholeModule: boolean;
    description?: string;
}

interface ViewMapping {
    srcLine: number;
    start: number;
    end: number;
    stack?: ViewFrame[];
}

interface ViewFrame {
    file: string;
    line: number;
    func: string;
    root: boolean;
}

interface ViewState {
    uri: vscode.Uri;
    sourceUri: string;
    kind: ViewKind;
    optimized: boolean;
    wholeModule: boolean;
    text: string;
    mappings: ViewMapping[];
    decoration: vscode.TextEditorDecorationType;
    outerDecoration: vscode.TextEditorDecorationType;
}

const viewChoices: ViewChoice[] = [
    { id: 'unoptimized', label: 'Unoptimized', optimized: false, wholeModule: false },
    { id: 'optimized', label: 'Optimized -O2', optimized: true, wholeModule: false },
    {
        id: 'whole-unoptimized',
        label: 'Whole module (unoptimized)',
        optimized: false,
        wholeModule: true,
        description: 'Includes imported/core code'
    },
    {
        id: 'whole-optimized',
        label: 'Whole module -O2',
        optimized: true,
        wholeModule: true,
        description: 'Includes imported/core code'
    }
];
let lastViewChoiceId: string | undefined;

// How far from the cursor the jump heuristic will look for a line that produced code.
// Beyond this the nearest mapping is more likely a different function than the one asked for.
const maxJumpLineDistance = 40;

class CflatViewContentProvider implements vscode.TextDocumentContentProvider {
    private readonly changeEmitter = new vscode.EventEmitter<vscode.Uri>();
    readonly onDidChange = this.changeEmitter.event;
    private readonly states = new Map<string, ViewState>();
    private readonly statusBar =
        vscode.window.createStatusBarItem(vscode.StatusBarAlignment.Right, 90);

    constructor(private readonly getClient: () => LanguageClient | undefined) {}

    createView(sourceUri: string, kind: ViewKind, choice: ViewChoice): vscode.Uri {
        const key = `${sourceUri}|${kind}|${choice.id}`;
        // Name the tab after the source file with the output extension (foo.cb -> foo.ll / foo.s).
        const sourceName = path.basename(vscode.Uri.parse(sourceUri).fsPath).replace(/\.[^.]+$/, '');
        const uri = vscode.Uri.from({
            scheme: 'cflat-view',
            path: `/${sourceName}${kind === 'ir' ? '.ll' : '.s'}`,
            query: `key=${encodeURIComponent(key)}`
        });
        const oldState = this.states.get(uri.toString());
        oldState?.decoration.dispose();
        oldState?.outerDecoration.dispose();
        this.states.set(uri.toString(), {
            uri,
            sourceUri,
            kind,
            optimized: choice.optimized,
            wholeModule: choice.wholeModule,
            text: '',
            mappings: [],
            decoration: vscode.window.createTextEditorDecorationType({
                backgroundColor: new vscode.ThemeColor('editor.rangeHighlightBackground')
            }),
            outerDecoration: vscode.window.createTextEditorDecorationType({
                backgroundColor: new vscode.ThemeColor('editor.wordHighlightBackground')
            })
        });
        return uri;
    }

    async provideTextDocumentContent(uri: vscode.Uri): Promise<string> {
        const state = this.states.get(uri.toString());
        if (!state) return '';
        if (state.text === '') await this.refresh(uri);
        return state.text;
    }

    async refresh(uri: vscode.Uri): Promise<void> {
        const state = this.states.get(uri.toString());
        const activeClient = this.getClient();
        if (!state || !activeClient) return;
        const params: {
            uri: string;
            kind: ViewKind;
            optimized: boolean;
            optLevel: 0 | 2;
            wholeModule?: boolean;
        } = {
            uri: state.sourceUri,
            kind: state.kind,
            optimized: state.optimized,
            optLevel: state.optimized ? 2 : 0,
            ...(state.wholeModule ? { wholeModule: true } : {})
        };
        try {
            const result = await activeClient.sendRequest<{
                kind: string;
                text: string;
                mappings?: ViewMapping[];
                cached?: boolean;
                timings?: { analyzeMs?: number; emitMs?: number; totalMs?: number };
            }>(
                'cflat/viewAssembly', params);
            this.showTimings(state, result.timings, result.cached === true);
            state.text = result.text;
            state.mappings = Array.isArray(result.mappings)
                ? result.mappings.reduce<ViewMapping[]>((valid, mapping) => {
                    if (!mapping || !Number.isInteger(mapping.srcLine) ||
                        !Number.isInteger(mapping.start) || !Number.isInteger(mapping.end) ||
                        mapping.srcLine <= 0 || mapping.start <= 0 || mapping.end < mapping.start) {
                        return valid;
                    }
                    const stack = Array.isArray(mapping.stack) && mapping.stack.every(frame =>
                        frame && typeof frame.file === 'string' && Number.isInteger(frame.line) &&
                        frame.line > 0 && typeof frame.func === 'string' &&
                        typeof frame.root === 'boolean') ? mapping.stack : undefined;
                    valid.push({
                        srcLine: mapping.srcLine,
                        start: mapping.start,
                        end: mapping.end,
                        ...(stack ? { stack } : {})
                    });
                    return valid;
                }, [])
                : [];
        } catch (error) {
            state.text = `; view request failed\n; ${String(error)}`;
            state.mappings = [];
        }
        this.changeEmitter.fire(uri);
        this.refreshHighlights();
    }

    async refreshSource(sourceUri: string): Promise<void> {
        const refreshes: Thenable<void>[] = [];
        for (const state of this.states.values()) {
            if (state.sourceUri !== sourceUri) continue;
            const visible = vscode.window.visibleTextEditors.some(
                editor => editor.document.uri.toString() === state.uri.toString());
            if (visible) refreshes.push(this.refresh(state.uri));
        }
        await Promise.all(refreshes);
    }

    hasViewsFor(sourceUri: string): boolean {
        for (const state of this.states.values()) {
            if (state.sourceUri === sourceUri) return true;
        }
        return false;
    }

    closeView(uri: vscode.Uri): void {
        const key = uri.toString();
        const state = this.states.get(key);
        if (!state) return;
        state.decoration.dispose();
        state.outerDecoration.dispose();
        this.states.delete(key);
        if (this.states.size === 0) this.statusBar.hide();
    }

    updateSelection(editor: vscode.TextEditor): void {
        const viewState = this.states.get(editor.document.uri.toString());
        if (viewState) {
            this.clearSourceHighlights(viewState.sourceUri);
            this.clearViewHighlights(viewState);
            const mapping = viewState.mappings.find(candidate =>
                editor.selection.active.line + 1 >= candidate.start &&
                editor.selection.active.line + 1 <= candidate.end);
            if (!mapping) return;
            const sourceEditor = vscode.window.visibleTextEditors.find(candidate =>
                candidate.document.uri.toString() === viewState.sourceUri);
            if (!sourceEditor) return;
            const rootFrames = mapping.stack?.filter(frame => frame.root) ?? [];
            const sourceLines = rootFrames.length > 0
                ? rootFrames.map(frame => frame.line)
                : [mapping.srcLine];
            const ranges = sourceLines
                .map(line => this.lineRange(sourceEditor.document, line))
                .filter((range): range is vscode.Range => range !== undefined);
            if (ranges.length === 0) return;
            sourceEditor.setDecorations(viewState.decoration, [ranges[0]]);
            sourceEditor.setDecorations(viewState.outerDecoration, ranges.slice(1));
            sourceEditor.revealRange(ranges[0], vscode.TextEditorRevealType.InCenterIfOutsideViewport);
            if (mapping.stack && mapping.stack.length > 1) {
                const description = mapping.stack.map(frame =>
                    frame.file + ':' + frame.line + ' (' + frame.func + ')').join(' <- inlined at ');
                vscode.window.setStatusBarMessage(description, 5000);
            }
            return;
        }

        const states = [...this.states.values()].filter(state => state.sourceUri === editor.document.uri.toString());
        for (const state of states) {
            this.clearSourceHighlights(state.sourceUri);
            this.clearViewHighlights(state);
            this.revealInView(state, editor.selection.active.line + 1);
        }
    }

    // Move a just-opened view to the output generated for the given source line, using the
    // debug-info line mappings the server returned.
    revealSourceLine(viewUri: vscode.Uri, sourceLine: number): void {
        const state = this.states.get(viewUri.toString());
        if (!state || this.revealInView(state, sourceLine, true)) return;

        // Blank lines, comments, braces and declarations emit no code of their own, so an exact
        // match often does not exist. Land on the closest line that did emit code instead of
        // leaving the view where it opened.
        const nearest = this.nearestMappedLine(state, sourceLine);
        if (nearest === undefined || !this.revealInView(state, nearest, true)) return;
        vscode.window.setStatusBarMessage(state.kind === 'ir'
            ? l10n.t({
                message: 'cflat: line {0} produced no IR - showing the nearest emitted line {1}',
                args: [sourceLine, nearest],
                comment: ['Status bar message. IR = LLVM Intermediate Representation; keep "IR" as-is.',
                    '{0} and {1} are source line numbers.']
            })
            : l10n.t({
                message: 'cflat: line {0} produced no assembly - showing the nearest emitted line {1}',
                args: [sourceLine, nearest],
                comment: ['Status bar message. "assembly" means the emitted machine-code listing.',
                    '{0} and {1} are source line numbers.']
            }), 5000);
    }

    // Closest emitted line to the cursor, preferring the one below when two are equidistant:
    // that puts a function signature on its own body rather than on the function above it.
    private nearestMappedLine(state: ViewState, sourceLine: number): number | undefined {
        let best: number | undefined;
        let bestDistance = Number.POSITIVE_INFINITY;
        for (const line of this.mappedSourceLines(state)) {
            const distance = Math.abs(line - sourceLine);
            if (distance > maxJumpLineDistance) continue;
            if (distance < bestDistance || (distance === bestDistance && line > sourceLine)) {
                best = line;
                bestDistance = distance;
            }
        }
        return best;
    }

    private mappedSourceLines(state: ViewState): Set<number> {
        const lines = new Set<number>();
        for (const mapping of state.mappings) {
            lines.add(mapping.srcLine);
            for (const frame of mapping.stack ?? [])
                if (frame.root) lines.add(frame.line);
        }
        return lines;
    }

    private revealInView(state: ViewState, sourceLine: number, moveCursor = false): boolean {
        const ranges = state.mappings
            .filter(mapping => mapping.srcLine === sourceLine ||
                mapping.stack?.some(frame => frame.root && frame.line === sourceLine))
            .flatMap(mapping => this.viewRanges(state, mapping));
        const viewEditor = vscode.window.visibleTextEditors.find(candidate =>
            candidate.document.uri.toString() === state.uri.toString());
        if (!viewEditor || ranges.length === 0) return false;
        viewEditor.setDecorations(state.decoration, ranges);
        if (moveCursor) viewEditor.selection = new vscode.Selection(ranges[0].start, ranges[0].start);
        viewEditor.revealRange(ranges[0], vscode.TextEditorRevealType.InCenterIfOutsideViewport);
        return true;
    }

    private clearSourceHighlights(sourceUri: string): void {
        for (const editor of vscode.window.visibleTextEditors) {
            if (editor.document.uri.toString() !== sourceUri) continue;
            for (const state of this.states.values()) {
                if (state.sourceUri === sourceUri) {
                    editor.setDecorations(state.decoration, []);
                    editor.setDecorations(state.outerDecoration, []);
                }
            }
        }
    }

    private clearViewHighlights(state: ViewState): void {
        for (const editor of vscode.window.visibleTextEditors) {
            if (editor.document.uri.toString() === state.uri.toString()) {
                editor.setDecorations(state.decoration, []);
            }
        }
    }

    private lineRange(document: vscode.TextDocument, line: number): vscode.Range | undefined {
        const lineIndex = line - 1;
        if (lineIndex < 0 || lineIndex >= document.lineCount) return undefined;
        return new vscode.Range(lineIndex, 0, lineIndex, document.lineAt(lineIndex).text.length);
    }

    private viewRanges(state: ViewState, mapping: ViewMapping): vscode.Range[] {
        const viewEditor = vscode.window.visibleTextEditors.find(editor =>
            editor.document.uri.toString() === state.uri.toString());
        if (!viewEditor) return [];
        const start = Math.max(1, mapping.start);
        const end = Math.min(viewEditor.document.lineCount, mapping.end);
        const ranges: vscode.Range[] = [];
        for (let line = start; line <= end; line++) {
            const range = this.lineRange(viewEditor.document, line);
            if (range) ranges.push(range);
        }
        return ranges;
    }

    private refreshHighlights(): void {
        const editor = vscode.window.activeTextEditor;
        if (editor) this.updateSelection(editor);
    }

    // Status-bar line for the last view refresh, so a slow request is self-explaining.
    private showTimings(state: ViewState,
        timings: { analyzeMs?: number; emitMs?: number; totalMs?: number } | undefined,
        cached: boolean): void {
        const label = `${state.kind === 'ir' ? 'IR' : 'asm'}${state.optimized ? ' -O2' : ''}`;
        if (cached || !timings) {
            this.statusBar.text = l10n.t({
                message: '$(watch) {0} view: cached',
                args: [label],
                comment: ['$(watch) is a VS Code icon token, not text. Leave it exactly as written, including the parentheses.',
                    '{0} is a view label such as "IR" or "asm -O2", produced by the extension and not translated.']
            });
            this.statusBar.tooltip = l10n.t({
                message: 'cflat view served from the response cache',
                comment: ['Status bar tooltip: the shown output was reused, not recomputed.']
            });
        } else {
            const analyze = timings.analyzeMs ?? 0;
            const emit = timings.emitMs ?? 0;
            const total = timings.totalMs ?? analyze + emit;
            this.statusBar.text = l10n.t({
                message: '$(watch) {0} view: {1}ms',
                args: [label, total],
                comment: ['$(watch) is a VS Code icon token, not text. Leave it exactly as written, including the parentheses.',
                    '{0} is a view label such as "IR" or "asm -O2", produced by the extension and not translated.',
                    'ms = milliseconds.']
            });
            this.statusBar.tooltip = analyze === 0
                ? l10n.t({
                    message: 'cflat view refresh: analyze {0}ms + emit {1}ms (analysis reused)',
                    args: [analyze, emit],
                    comment: ['Status bar tooltip. ms = milliseconds.',
                        '"analysis reused" means the previous parse was still valid.']
                })
                : l10n.t({
                    message: 'cflat view refresh: analyze {0}ms + emit {1}ms',
                    args: [analyze, emit],
                    comment: ['Status bar tooltip. ms = milliseconds.']
                });
        }
        this.statusBar.show();
    }

    dispose(): void {
        for (const state of this.states.values()) {
            state.decoration.dispose();
            state.outerDecoration.dispose();
        }
        this.changeEmitter.dispose();
        this.statusBar.dispose();
    }
}

// Resolve the compiler exe: an explicit cflat.executablePath setting always wins; otherwise
// fall back to the path that `cflat --init` records in ~/.cflat/compiler_path.txt.
function findCompilerExecutable(): string | undefined {
    const configured = vscode.workspace.getConfiguration('cflat').get<string>('executablePath');
    if (configured && configured.trim() !== '') {
        return configured.trim();
    }
    return readInitRecordedCompilerPath();
}

function readInitRecordedCompilerPath(): string | undefined {
    const home = process.env.USERPROFILE ?? process.env.HOME;
    if (!home) {
        return undefined;
    }
    const recordPath = path.join(home, '.cflat', 'compiler_path.txt');
    try {
        const exePath = fs.readFileSync(recordPath, 'utf8').trim();
        if (exePath !== '' && fs.existsSync(exePath)) {
            return exePath;
        }
    } catch {
        // No record file (or unreadable) - fall through to undefined.
    }
    return undefined;
}

async function startClient(): Promise<void> {
    const exePath = findCompilerExecutable();
    if (!exePath) {
        outputChannel.appendLine('ERROR: could not locate the cflat compiler.');
        outputChannel.show(true);
        vscode.window.showWarningMessage(l10n.t(
            'cflat: could not locate the cflat compiler (cflat.exe on Windows, cflat on macOS/Linux). ' +
            'Run "cflat --init" to record the compiler path, ' +
            'or set cflat.executablePath in Settings to enable the language server.'
        ));
        return;
    }

    outputChannel.appendLine(`Compiler exe   : ${exePath}`);

    const coreDir = path.join(path.dirname(exePath), 'core');
    const runtimeCb = path.join(coreDir, 'runtime.cb');
    outputChannel.appendLine(`Core directory : ${coreDir} (${fs.existsSync(coreDir) ? 'exists' : 'MISSING'})`);
    outputChannel.appendLine(`runtime.cb     : ${runtimeCb} (${fs.existsSync(runtimeCb) ? 'exists' : 'MISSING'})`);

    const serverOptions: ServerOptions = {
        run:   { command: exePath, args: ['lsp'],                                          transport: TransportKind.stdio },
        debug: { command: exePath, args: ['lsp', '--verbose', '--log-file', logFilePath],  transport: TransportKind.stdio }
    };

    // The client library already sends initialize.params.locale; pass it again in
    // initializationOptions so the server sees it on clients that omit the field.
    outputChannel.appendLine(`UI language    : ${vscode.env.language}`);

    const clientOptions: LanguageClientOptions = {
        documentSelector: [{ scheme: 'file', language: 'cflat' }],
        synchronize: {
            fileEvents: vscode.workspace.createFileSystemWatcher('**/*.{cb,c}')
        },
        initializationOptions: { locale: vscode.env.language },
        outputChannel
    };

    client = new LanguageClient(
        'cflatLanguageServer',
        'cflat Language Server',
        serverOptions,
        clientOptions
    );

    outputChannel.appendLine('Starting LSP client...');
    await client.start();
    outputChannel.appendLine('LSP client started.');
}

async function restartClient(reason: string): Promise<void> {
    outputChannel.appendLine(`Restarting LSP client: ${reason}`);
    if (client) {
        try {
            await client.stop();
        } catch (err) {
            outputChannel.appendLine(`Error stopping LSP client: ${err}`);
        }
        client = undefined;
    }
    await startClient();
}

// F5 on a .cb file routes to type 'cflat'; resolve compiles with -g and returns a cppvsdbg
// config so the C/C++ extension drives the actual debug session.
class CflatDebugConfigurationProvider implements vscode.DebugConfigurationProvider {
    provideDebugConfigurations(): vscode.DebugConfiguration[] {
        return [{
            type: 'cflat',
            request: 'launch',
            name: 'Run cflat file',
            program: '${file}',
            args: [],
            stopAtEntry: false
        }];
    }

    async resolveDebugConfigurationWithSubstitutedVariables(
        _folder: vscode.WorkspaceFolder | undefined,
        config: vscode.DebugConfiguration
    ): Promise<vscode.DebugConfiguration | undefined> {
        // Prefer config.program; fall back to the active editor. In either case the source
        // must end with .cb or .c - otherwise we would compile an unrelated file (e.g. the
        // previously produced .exe if ${file} happened to resolve to it).
        const isSource = (p: string | undefined): p is string =>
            typeof p === 'string' && /\.(cb|c)$/i.test(p);

        let source: string | undefined = isSource(config.program) ? config.program : undefined;
        if (!source) {
            const editor = vscode.window.activeTextEditor;
            if (editor && isSource(editor.document.fileName)) {
                source = editor.document.fileName;
            }
        }
        if (!source) {
            vscode.window.showErrorMessage(l10n.t(
                'cflat: no .cb / .c source file to debug. ' +
                'Open the source file in the active editor, or set "program" in launch.json to a .cb path.'
            ));
            return undefined;
        }

        const cflatExe = findCompilerExecutable();
        if (!cflatExe) {
            vscode.window.showErrorMessage(l10n.t({
                message: 'cflat: cflat.executablePath is not set. Set it in Settings to enable debugging.',
                comment: ['cflat.executablePath is a settings identifier - never translate it.',
                    '"Settings" is the VS Code settings editor; use its localized name.']
            }));
            return undefined;
        }

        // On Windows the output keeps the .exe extension; elsewhere it has none. Either way
        // the output name differs from the source (.cb/.c), so it never collides on disk.
        const outExe = process.platform === 'win32'
            ? source.replace(/\.(cb|c)$/i, '.exe')
            : source.replace(/\.(cb|c)$/i, '');
        const ok = await compileForDebug(cflatExe, source, outExe);
        if (!ok) {
            return undefined;
        }

        // Prefer cpptools if installed: cppvsdbg on Windows, cppdbg+lldb/gdb elsewhere.
        // Otherwise fall back to running the program in an integrated terminal - no
        // breakpoints, but at least F5 still launches the program.
        const cpptools = vscode.extensions.getExtension('ms-vscode.cpptools');
        if (cpptools) {
            // Force-activate to avoid racing the C/C++ extension's lazy activation, which
            // would fail with "Couldn't find a debug adapter descriptor for 'cppvsdbg'".
            if (!cpptools.isActive) {
                await cpptools.activate();
            }
            if (process.platform === 'win32') {
                return {
                    name: config.name ?? `cflat: ${path.basename(outExe)}`,
                    type: 'cppvsdbg',
                    request: 'launch',
                    program: outExe,
                    args: config.args ?? [],
                    cwd: config.cwd ?? path.dirname(outExe),
                    stopAtEntry: config.stopAtEntry ?? false,
                    console: config.console ?? 'integratedTerminal'
                };
            }
            return {
                name: config.name ?? `cflat: ${path.basename(outExe)}`,
                type: 'cppdbg',
                request: 'launch',
                program: outExe,
                args: config.args ?? [],
                cwd: config.cwd ?? path.dirname(outExe),
                stopAtEntry: config.stopAtEntry ?? false,
                MIMode: process.platform === 'darwin' ? 'lldb' : 'gdb',
                externalConsole: false
            };
        }

        runInTerminal(outExe, config.args ?? [], config.cwd ?? path.dirname(outExe));
        return undefined;
    }
}

function runInTerminal(exePath: string, args: string[], cwd: string): void {
    const name = `cflat: ${path.basename(exePath)}`;
    const existing = vscode.window.terminals.find(t => t.name === name);
    const terminal = existing ?? vscode.window.createTerminal({ name, cwd });
    terminal.show(true);
    const quoted = [exePath, ...args].map(a => /\s/.test(a) ? `"${a}"` : a).join(' ');
    terminal.sendText(quoted);
}

function compileForDebug(cflatExe: string, source: string, outExe: string): Thenable<boolean> {
    return vscode.window.withProgress(
        {
            location: vscode.ProgressLocation.Window,
            title: l10n.t({
                message: 'cflat: compiling {0}...',
                args: [path.basename(source)],
                comment: ['Progress message. {0} is a source file name.']
            })
        },
        () => new Promise<boolean>(resolve => {
            outputChannel.appendLine(`Compiling: ${cflatExe} "${source}" -o "${outExe}" -g`);
            const proc = childProcess.spawn(
                cflatExe,
                [source, '-o', outExe, '-g'],
                { cwd: path.dirname(source) }
            );
            proc.stdout.on('data', d => outputChannel.append(d.toString()));
            proc.stderr.on('data', d => outputChannel.append(d.toString()));
            proc.on('error', err => {
                outputChannel.appendLine(`Compile error: ${err.message}`);
                outputChannel.show(true);
                vscode.window.showErrorMessage(
                    l10n.t({
                        message: 'cflat: failed to launch compiler ({0}).',
                        args: [err.message],
                        comment: ['{0} is an operating-system error message, already in English.']
                    }));
                resolve(false);
            });
            proc.on('close', code => {
                if (code === 0) {
                    outputChannel.appendLine('Compile OK.');
                    resolve(true);
                } else {
                    outputChannel.appendLine(`Compile failed (exit ${code}).`);
                    outputChannel.show(true);
                    vscode.window.showErrorMessage(l10n.t({
                        message: 'cflat: compile failed. See "cflat Language Server" output for details.',
                        comment: ['"cflat Language Server" is the name of an output panel - translate it the same way everywhere or leave it as-is.']
                    }));
                    resolve(false);
                }
            });
        })
    );
}

// Range covering the call itself, so the References peek highlights the call rather than
// column 0. LLVM reports every inline remark at column 0, so the column has to be
// recovered from the text; when the name is not on the line (a macro-ish expansion, or an
// edit since the measurement) the whole line is a truthful fallback.
function callRange(document: vscode.TextDocument, srcLine: number, calleeName: string): vscode.Range {
    const line = document.lineAt(srcLine - 1);
    const column = line.text.indexOf(calleeName);
    if (column < 0) return line.range;
    return new vscode.Range(srcLine - 1, column, srcLine - 1, column + calleeName.length);
}

// The detail behind the CodeLens. A CodeLens tooltip is a plain string - no markdown, no
// links - so the lens click opens a real hover here instead, where a trusted MarkdownString
// can carry the "Show IR" command link.
class CflatOptimizationHoverProvider implements vscode.HoverProvider {
    constructor(private readonly provider: CflatOptimizationLensProvider) {}

    provideHover(document: vscode.TextDocument, position: vscode.Position): vscode.Hover | undefined {
        const entry = this.provider.annotationSource(document);
        if (!entry) return undefined;
        const info = entry.functions.find(candidate => candidate.startLine === position.line + 1);
        if (!info) {
            // Not a definition line: a call, if the cursor sits on a callee the optimizer
            // decided about. VS Code merges this with the server's signature hover.
            const word = document.getWordRangeAtPosition(position);
            if (!word) return undefined;
            const lines = buildCallSiteHover(entry, position.line + 1, document.getText(word));
            if (lines.length === 0) return undefined;
            return new vscode.Hover(new vscode.MarkdownString(lines.join('\n')), word);
        }

        const markdown = new vscode.MarkdownString(
            buildFunctionDetail(entry, info).join('\n'));
        // Required for command: links to be clickable. The content is built from our own
        // server's response, never from anything the user typed.
        markdown.isTrusted = true;
        const uri = document.uri.toString();
        const links = [];
        const sites = collectInlineCallSites(entry, info);
        if (sites.length > 0) {
            const siteArgs = encodeURIComponent(JSON.stringify([uri, info.startLine]));
            links.push(`[${l10n.t({
                message: 'Show {0}',
                args: [countCallSites(sites.length)],
                comment: ['Link label in a hover. {0} arrives already formatted, e.g. "3 call sites".',
                    'Clicking the link lists those call sites.']
            })}]` + `(command:cflat.showInlineCallSites?${siteArgs})`);
        }
        // Both output forms are offered here, not just IR. They are the two things the
        // counters above are measured from - bytes and machine instructions come from the
        // assembly, the inline decisions from the IR - so hiding one behind the command
        // palette made the shorter path to it invisible.
        const viewArgs = (kind: ViewKind) => encodeURIComponent(
            JSON.stringify([uri, info.startLine, entry.optLevel > 0, kind]));
        links.push(`[${l10n.t({
            message: 'Show IR',
            comment: ['Link label. IR = LLVM Intermediate Representation; keep "IR" as-is.']
        })}](command:cflat.showViewForFunction?${viewArgs('ir')})`);
        links.push(`[${l10n.t({
            message: 'Show assembly',
            comment: ['Link label. "assembly" means the emitted machine-code listing.']
        })}](command:cflat.showViewForFunction?${viewArgs('asm')})`);
        markdown.appendMarkdown(`\n\n${links.join(' \u00b7 ')}`);
        return new vscode.Hover(markdown, document.lineAt(position.line).range);
    }
}

// Optimization facts above each function. The optimizer only runs on an explicit refresh
// (command or save) - provideCodeLenses never triggers one, since it fires on every edit.
class CflatOptimizationLensProvider implements vscode.CodeLensProvider {
    private readonly changeEmitter = new vscode.EventEmitter<void>();
    readonly onDidChangeCodeLenses = this.changeEmitter.event;
    private readonly cache = new Map<string, OptInfoCacheEntry>();
    private readonly inFlight = new Set<string>();

    constructor(private readonly getClient: () => LanguageClient | undefined) {}

    private static enabled(): boolean {
        return vscode.workspace.getConfiguration('cflat').get<boolean>('optimizationInfo.enable', false);
    }

    private static optLevel(): number {
        const level = vscode.workspace.getConfiguration('cflat').get<number>('optimizationInfo.optLevel', 2);
        return level === 0 || level === 1 || level === 2 ? level : 2;
    }

    private static cacheKey(document: vscode.TextDocument): string {
        return `${CflatOptimizationLensProvider.optLevel()}|${document.version}`;
    }

    // Cached facts for a document, or undefined when there are none for its CURRENT
    // version - an edited document's old measurements describe code that no longer exists.
    annotationSource(document: vscode.TextDocument): OptInfoCacheEntry | undefined {
        if (!CflatOptimizationLensProvider.enabled()) return undefined;
        if (!vscode.workspace.getConfiguration('cflat')
                .get<boolean>('optimizationInfo.inlineAnnotations', true)) return undefined;
        const entry = this.cache.get(document.uri.toString());
        if (!entry || entry.key !== CflatOptimizationLensProvider.cacheKey(document)) return undefined;
        return entry;
    }

    provideCodeLenses(document: vscode.TextDocument): vscode.CodeLens[] {
        if (!CflatOptimizationLensProvider.enabled()) return [];
        const entry = this.cache.get(document.uri.toString());
        if (!entry || entry.key !== CflatOptimizationLensProvider.cacheKey(document)) return [];

        const lenses: vscode.CodeLens[] = [];
        for (const info of entry.functions) {
            const line = info.startLine - 1;
            if (line < 0 || line >= document.lineCount) continue;
            const range = document.lineAt(line).range;
            lenses.push(new vscode.CodeLens(range, {
                title: this.lensTitle(info, entry.optLevel),
                tooltip: this.lensTooltip(info, entry.optLevel),
                command: 'cflat.showOptimizationDetail',
                arguments: [document.uri.toString(), info.startLine]
            }));
        }
        return lenses;
    }

    private lensTitle(info: OptFunctionInfo, optLevel: number): string {
        const prefix = `O${optLevel}: `;
        if (info.eliminated) {
            return prefix + (info.inlinedInto > 0
                ? l10n.t({
                    message: 'optimized away - inlined into {0}',
                    args: [countFunctions(info.inlinedInto)],
                    comment: ['No standalone copy of the function was emitted; its body was copied into its callers.',
                        '{0} arrives already formatted, e.g. "3 functions".',
                        'Appears in a one-line CodeLens above a function, so keep it short.']
                })
                : l10n.t({
                    message: 'optimized away',
                    comment: ['The optimizer emitted no code for this function.',
                        'Appears in a one-line CodeLens above a function, so keep it short.']
                }));
        }

        // Emitted bytes leads: it is the figure that means something outside the compiler.
        // The IR instruction count is not shown at all - see buildFunctionDetail.
        const parts: string[] = [];
        if (info.bytes > 0) parts.push(`${info.bytes} B`);
        if (info.machineInstructions > 0) parts.push(countInstructions(info.machineInstructions));
        if (info.stackBytes > 0) parts.push(l10n.t({
            message: '{0} B stack',
            args: [info.stackBytes],
            comment: ['B = bytes. Stack space reserved by the function.',
                'Appears in a one-line CodeLens above a function, so keep it short.']
        }));
        if (info.spills > 0) {
            parts.push(info.reloads > 0
                ? `${countSpills(info.spills)}/${countReloads(info.reloads)}`
                : countSpills(info.spills));
        }
        if (info.inlinedInto > 0)
            parts.push(l10n.t({
                message: 'inlined into {0}',
                args: [countFunctions(info.inlinedInto)],
                comment: ['{0} arrives already formatted, e.g. "3 functions".',
                    'Appears in a one-line CodeLens above a function, so keep it short.']
            }));
        // The IR count used to guarantee a part; without it every counter can be
        // unavailable at once, and a lens reading just "O2: " would look broken.
        if (parts.length === 0) parts.push(l10n.t({
            message: 'no size information',
            comment: ['Shown when the server reported no counters at all.',
                'Appears in a one-line CodeLens above a function, so keep it short.']
        }));
        return prefix + parts.join(' - ');
    }

    private lensTooltip(info: OptFunctionInfo, optLevel: number): string {
        const lines = [l10n.t({
            message: '{0} at -O{1}',
            args: [info.name, optLevel],
            comment: ['{0} is a function name (never translate it).',
                '-O0 / -O1 / -O2 is a compiler optimization-level flag. Never translate it and never separate the O from its digit.']
        })];
        if (info.eliminated) lines.push(l10n.t({
            message: 'No code emitted (inlined or removed)',
            comment: ['CodeLens tooltip line for a function the optimizer did not emit.']
        }));
        else {
            if (info.bytes > 0) lines.push(l10n.t({
                message: 'Function size: {0} bytes',
                args: [info.bytes],
                comment: ['Size of this function\'s machine code in the object file.']
            }));
            if (info.machineInstructions > 0)
                lines.push(l10n.t({
                    message: 'Machine instructions: {0}',
                    args: [info.machineInstructions],
                    comment: ['Count of CPU instructions emitted for this function.']
                }));
            if (info.stackBytes > 0) lines.push(l10n.t({
                message: 'Stack frame: {0} bytes',
                args: [info.stackBytes],
                comment: ['Stack space this function reserves for its locals and spills.']
            }));
            lines.push(l10n.t({
                message: 'Register: {0}, {1}',
                args: [countSpills(info.spills), countReloads(info.reloads)],
                comment: ['Register allocator counters; both arrive already formatted, e.g. "2 spills" and "1 reload".']
            }));
        }
        if (info.inlinedInto > 0)
            lines.push(l10n.t({
                message: 'Body inlined into {0}',
                args: [countFunctions(info.inlinedInto)],
                comment: ['{0} arrives formatted, e.g. "3 functions".']
            }));
        lines.push(l10n.t({
            message: 'Click for the full detail, including every inline decision',
            comment: ['Last line of the CodeLens tooltip; clicking opens a fuller hover.']
        }));
        return lines.join('\n');
    }

    // True when the cache already holds measurements for this document's CURRENT version
    // and opt level, i.e. a round trip would return what we have.
    private isFresh(document: vscode.TextDocument): boolean {
        const entry = this.cache.get(document.uri.toString());
        return entry !== undefined && entry.key === CflatOptimizationLensProvider.cacheKey(document);
    }

    // The editor-switch path. Collecting optimization info runs the whole -O2 pipeline, so
    // unlike refresh() this is a no-op when the cache is already current - otherwise tabbing
    // back and forth between two files would re-optimize both on every switch.
    async refreshIfStale(document: vscode.TextDocument): Promise<void> {
        if (this.isFresh(document)) return;
        await this.refresh(document, true);
    }

    async refresh(document: vscode.TextDocument, silent = false): Promise<void> {
        if (!CflatOptimizationLensProvider.enabled() || document.languageId !== 'cflat') return;
        const client = this.getClient();
        if (!client) return;

        const uri = document.uri.toString();
        if (this.inFlight.has(uri)) return;
        this.inFlight.add(uri);
        const optLevel = CflatOptimizationLensProvider.optLevel();
        const key = CflatOptimizationLensProvider.cacheKey(document);
        try {
            const result = await client.sendRequest<OptInfoResult>('cflat/optimizationInfo',
                { uri, optLevel, remarks: true });
            this.cache.set(uri, {
                key, optLevel: result.optLevel,
                functions: result.functions ?? [],
                remarks: result.remarks ?? []
            });
            this.changeEmitter.fire();
            if (!silent) {
                vscode.window.setStatusBarMessage(
                    l10n.t({
                        message: 'cflat: optimization info updated at -O{0}',
                        args: [result.optLevel],
                        comment: ['-O0 / -O1 / -O2 is a compiler optimization-level flag. Never translate it and never separate the O from its digit.']
                    }), 4000);
            }
        } catch (error) {
            this.cache.delete(uri);
            this.changeEmitter.fire();
            if (!silent) {
                vscode.window.showWarningMessage(
                    l10n.t({
                        message: 'cflat: could not collect optimization info - {0}',
                        args: [String(error)],
                        comment: ['{0} is an error message from the language server, already in English.']
                    }));
            }
        } finally {
            this.inFlight.delete(uri);
        }
    }

    invalidate(uri?: string): void {
        if (uri) this.cache.delete(uri);
        else this.cache.clear();
        this.changeEmitter.fire();
    }

    dispose(): void {
        this.changeEmitter.dispose();
    }
}

async function showCompilerView(provider: CflatViewContentProvider, kind: ViewKind): Promise<void> {
    const editor = vscode.window.activeTextEditor;
    if (!editor || editor.document.languageId !== 'cflat' || !client) return;

    const activeChoice = viewChoices.find(choice => choice.id === lastViewChoiceId);
    const quickPick = vscode.window.createQuickPick<ViewChoice>();
    quickPick.items = viewChoices;
    quickPick.placeholder = 'Choose the compiler view';
    if (activeChoice) quickPick.activeItems = [activeChoice];
    const choice = await new Promise<ViewChoice | undefined>(resolve => {
        let settled = false;
        quickPick.onDidAccept(() => {
            settled = true;
            resolve(quickPick.selectedItems[0]);
            quickPick.hide();
        });
        quickPick.onDidHide(() => {
            if (!settled) resolve(undefined);
            quickPick.dispose();
        });
        quickPick.show();
    });
    if (!choice) return;
    lastViewChoiceId = choice.id;
    await openCompilerView(provider, kind, choice, editor.document, editor.selection.active.line + 1);
}

// Open (or re-open) a compiler view for a document and jump to the output for sourceLine.
async function openCompilerView(provider: CflatViewContentProvider, kind: ViewKind,
                                choice: ViewChoice, source: vscode.TextDocument,
                                sourceLine: number): Promise<void> {
    const viewUri = provider.createView(source.uri.toString(), kind, choice);
    await provider.refresh(viewUri);
    const document = await vscode.workspace.openTextDocument(viewUri);
    const languages = await vscode.languages.getLanguages();
    const language = kind === 'ir'
        ? (languages.includes('llvm') ? 'llvm' : 'plaintext')
        : (languages.includes('asm') ? 'asm' : 'plaintext');
    await vscode.languages.setTextDocumentLanguage(document, language);
    await vscode.window.showTextDocument(document, {
        viewColumn: vscode.ViewColumn.Beside,
        preserveFocus: false,
        preview: false
    });
    provider.revealSourceLine(viewUri, sourceLine);
}

export function activate(context: vscode.ExtensionContext): void {
    // Must be a log channel: vscode-languageclient 10 types clientOptions.outputChannel
    // as LogOutputChannel, which extends OutputChannel.
    // optimization_info.ts localizes through the standalone @vscode/l10n loader so it stays
    // unit-testable outside the extension host. Point it at the bundle VS Code already
    // resolved for the display language; undefined means English, which needs no bundle.
    if (vscode.l10n.bundle) l10n.config({ contents: vscode.l10n.bundle });

    outputChannel = vscode.window.createOutputChannel('cflat Language Server', { log: true });
    context.subscriptions.push(outputChannel);

    outputChannel.appendLine('=== cflat Extension Activating ===');
    outputChannel.appendLine(`Extension path : ${context.extensionPath}`);
    outputChannel.appendLine(`Log directory  : ${context.logUri.fsPath}`);

    const workspaceFolders = vscode.workspace.workspaceFolders ?? [];
    if (workspaceFolders.length === 0) {
        outputChannel.appendLine('Workspace folders: (none)');
    } else {
        workspaceFolders.forEach(f => outputChannel.appendLine(`Workspace folder: ${f.uri.fsPath}`));
    }

    const configured = vscode.workspace.getConfiguration('cflat').get<string>('executablePath');
    outputChannel.appendLine(`cflat.executablePath setting: "${configured ?? ''}"`);

    logFilePath = path.join(context.logUri.fsPath, 'lsp.log');

    const clientReady = startClient().catch(() => undefined);

    const viewProvider = new CflatViewContentProvider(() => client);
    const editRefreshTimers = new Map<string, ReturnType<typeof setTimeout>>();
    context.subscriptions.push(
        new vscode.Disposable(() => {
            for (const timer of editRefreshTimers.values()) clearTimeout(timer);
            editRefreshTimers.clear();
        }),
        vscode.workspace.registerTextDocumentContentProvider('cflat-view', viewProvider),
        viewProvider,
        vscode.window.onDidChangeTextEditorSelection(event => {
            viewProvider.updateSelection(event.textEditor);
        }),
        vscode.workspace.onDidCloseTextDocument(document => {
            if (document.uri.scheme === 'cflat-view') viewProvider.closeView(document.uri);
        }),
        vscode.workspace.onDidSaveTextDocument(document => {
            void viewProvider.refreshSource(document.uri.toString());
        }),
        vscode.workspace.onDidChangeTextDocument(event => {
            const document = event.document;
            if (document.uri.scheme !== 'file' || document.languageId !== 'cflat' ||
                event.contentChanges.length === 0) return;
            const sourceUri = document.uri.toString();
            if (!viewProvider.hasViewsFor(sourceUri)) return;
            const oldTimer = editRefreshTimers.get(sourceUri);
            if (oldTimer !== undefined) clearTimeout(oldTimer);
            const timer = setTimeout(() => {
                editRefreshTimers.delete(sourceUri);
                void viewProvider.refreshSource(sourceUri);
            }, 400);
            editRefreshTimers.set(sourceUri, timer);
        })
    );

    // Restart the LSP client when cflat.executablePath changes so the user
    // doesn't have to reload the window.
    context.subscriptions.push(
        vscode.workspace.onDidChangeConfiguration(e => {
            if (e.affectsConfiguration('cflat.executablePath')) {
                void restartClient('cflat.executablePath changed');
            }
        })
    );

    // Command: manually trigger diagnostics on current file
    context.subscriptions.push(
        vscode.commands.registerCommand('cflat.runDiagnostics', () => {
            const editor = vscode.window.activeTextEditor;
            if (editor && editor.document.languageId === 'cflat' && client) {
                client.sendNotification('cflat/runDiagnostics', {
                    uri: editor.document.uri.toString()
                });
            }
        })
    );

    context.subscriptions.push(
        vscode.commands.registerCommand('cflat.showLlvmIr', () => {
            void showCompilerView(viewProvider, 'ir');
        }),
        vscode.commands.registerCommand('cflat.showAssembly', () => {
            void showCompilerView(viewProvider, 'asm');
        })
    );

    // Optimization info: CodeLens above each function, refreshed on demand and on save.
    const lensProvider = new CflatOptimizationLensProvider(() => client);
    context.subscriptions.push(
        lensProvider,
        vscode.languages.registerCodeLensProvider({ scheme: 'file', language: 'cflat' }, lensProvider),
        vscode.languages.registerHoverProvider({ scheme: 'file', language: 'cflat' },
            new CflatOptimizationHoverProvider(lensProvider)),
        // Invoked from the optimization hover only, so deliberately not in the palette.
        vscode.commands.registerCommand('cflat.showViewForFunction',
            async (sourceUri: string, line: number, optimized: boolean, kind: ViewKind = 'ir') => {
                const choice = viewChoices.find(candidate =>
                    candidate.optimized === optimized && !candidate.wholeModule)
                    ?? viewChoices[0];
                const document = await vscode.workspace.openTextDocument(vscode.Uri.parse(sourceUri));
                await openCompilerView(viewProvider, kind, choice, document, line);
            }),
        // Target of the CodeLens click. A lens can only run a command, and VS Code has no
        // "show my tooltip" API, so put the cursor on the function and open the hover -
        // which is where the detail and the IR / Assembly links live.
        vscode.commands.registerCommand('cflat.showOptimizationDetail',
            async (sourceUri: string, line: number) => {
                const document = await vscode.workspace.openTextDocument(vscode.Uri.parse(sourceUri));
                const editor = await vscode.window.showTextDocument(document, { preserveFocus: false });
                const target = new vscode.Position(Math.max(0, line - 1), 0);
                editor.selection = new vscode.Selection(target, target);
                editor.revealRange(new vscode.Range(target, target));
                await vscode.commands.executeCommand('editor.action.showHover');
            }),
        // Hands the call sites to VS Code's own References peek rather than listing them
        // in the hover: a function called from 200 places needs a navigable, previewable
        // list, which is exactly what that widget already is.
        vscode.commands.registerCommand('cflat.showInlineCallSites',
            async (sourceUri: string, line: number) => {
                const uri = vscode.Uri.parse(sourceUri);
                const document = await vscode.workspace.openTextDocument(uri);
                const entry = lensProvider.annotationSource(document);
                const info = entry?.functions.find(candidate => candidate.startLine === line);
                if (!entry || !info) return;
                const locations = collectInlineCallSites(entry, info)
                    .filter(site => site.srcLine >= 1 && site.srcLine <= document.lineCount)
                    .map(site => new vscode.Location(uri, callRange(document, site.srcLine, info.name)));
                if (locations.length === 0) return;
                await vscode.window.showTextDocument(document, { preserveFocus: false });
                await vscode.commands.executeCommand('editor.action.showReferences',
                    uri, new vscode.Position(Math.max(0, line - 1), 0), locations);
            }),
        vscode.commands.registerCommand('cflat.showOptimizationInfo', () => {
            const editor = vscode.window.activeTextEditor;
            if (!editor || editor.document.languageId !== 'cflat') return;
            if (!vscode.workspace.getConfiguration('cflat').get<boolean>('optimizationInfo.enable', false)) {
                void vscode.window.showInformationMessage(l10n.t({
                    message: 'cflat: optimization info is disabled. Enable cflat.optimizationInfo.enable first.',
                    comment: ['cflat.optimizationInfo.enable is a settings identifier - never translate it.']
                }));
                return;
            }
            void lensProvider.refresh(editor.document);
        }),
        vscode.commands.registerCommand('cflat.toggleOptimizationInfo', async () => {
            const config = vscode.workspace.getConfiguration('cflat');
            const next = !config.get<boolean>('optimizationInfo.enable', false);
            await config.update('optimizationInfo.enable', next, vscode.ConfigurationTarget.Global);
            const editor = vscode.window.activeTextEditor;
            if (next && editor && editor.document.languageId === 'cflat')
                void lensProvider.refresh(editor.document);
        }),
        vscode.workspace.onDidSaveTextDocument((document: vscode.TextDocument) => {
            if (document.languageId === 'cflat') void lensProvider.refresh(document, true);
        }),
        // Opening or switching to a file must collect its measurements. provideCodeLenses
        // cannot do this - it fires on every edit, and running -O2 per keystroke is not
        // affordable - so the editor-switch event is the trigger, guarded by the cache.
        vscode.window.onDidChangeActiveTextEditor(editor => {
            if (editor?.document.languageId === 'cflat')
                void lensProvider.refreshIfStale(editor.document);
        }),
        vscode.workspace.onDidCloseTextDocument((document: vscode.TextDocument) => {
            lensProvider.invalidate(document.uri.toString());
        }),
        vscode.workspace.onDidChangeConfiguration((e: vscode.ConfigurationChangeEvent) => {
            if (!e.affectsConfiguration('cflat.optimizationInfo')) return;
            lensProvider.invalidate();
            const editor = vscode.window.activeTextEditor;
            if (editor && editor.document.languageId === 'cflat')
                void lensProvider.refresh(editor.document, true);
        })
    );

    // The editor open at activation never raises onDidChangeActiveTextEditor, and the
    // client is still starting at this point, so prime it once the server is up.
    void clientReady.then(() => {
        const editor = vscode.window.activeTextEditor;
        if (editor?.document.languageId === 'cflat')
            void lensProvider.refreshIfStale(editor.document);
    });

    // Command: show compiler output channel
    context.subscriptions.push(
        vscode.commands.registerCommand('cflat.showOutput', () => {
            (client?.outputChannel ?? outputChannel).show();
        })
    );

    // TriggerKind.Initial is invoked when populating launch.json (snippet flow).
    // TriggerKind.Dynamic is invoked when F5 is pressed with no launch.json - without this
    // registration F5-no-launch.json finds no configs and silently does nothing.
    const debugProvider = new CflatDebugConfigurationProvider();
    context.subscriptions.push(
        vscode.debug.registerDebugConfigurationProvider(
            'cflat', debugProvider, vscode.DebugConfigurationProviderTriggerKind.Initial
        ),
        vscode.debug.registerDebugConfigurationProvider(
            'cflat', debugProvider, vscode.DebugConfigurationProviderTriggerKind.Dynamic
        )
    );

    // Command: manually restart the LSP client
    context.subscriptions.push(
        vscode.commands.registerCommand('cflat.restartServer', () => {
            void restartClient('user invoked cflat.restartServer');
        })
    );

    context.subscriptions.push(
        vscode.window.setStatusBarMessage(l10n.t({
            message: 'cflat Language Server started',
            comment: ['Status bar message. "cflat" is the product name - never translate it.']
        }), 3000)
    );
}

export function deactivate(): Thenable<void> | undefined {
    if (!client) {
        return undefined;
    }
    return client.stop();
}
