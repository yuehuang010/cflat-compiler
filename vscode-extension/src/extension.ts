import * as childProcess from 'child_process';
import * as fs from 'fs';
import * as path from 'path';
import * as vscode from 'vscode';
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
    text: string;
    mappings: ViewMapping[];
    decoration: vscode.TextEditorDecorationType;
    outerDecoration: vscode.TextEditorDecorationType;
}

const viewChoices: ViewChoice[] = [
    { id: 'unoptimized', label: 'Not optimized', optimized: false },
    { id: 'optimized', label: 'Optimized (-O2)', optimized: true }
];
let lastViewChoiceId: string | undefined;

// How far from the cursor the jump heuristic will look for a line that produced code.
// Beyond this the nearest mapping is more likely a different function than the one asked for.
const maxJumpLineDistance = 40;

class CflatViewContentProvider implements vscode.TextDocumentContentProvider {
    private readonly changeEmitter = new vscode.EventEmitter<vscode.Uri>();
    readonly onDidChange = this.changeEmitter.event;
    private readonly states = new Map<string, ViewState>();

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
        } = {
            uri: state.sourceUri,
            kind: state.kind,
            optimized: state.optimized,
            optLevel: state.optimized ? 2 : 0
        };
        try {
            const result = await activeClient.sendRequest<{
                kind: string;
                text: string;
                mappings?: ViewMapping[];
            }>(
                'cflat/viewAssembly', params);
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

    closeView(uri: vscode.Uri): void {
        const key = uri.toString();
        const state = this.states.get(key);
        if (!state) return;
        state.decoration.dispose();
        state.outerDecoration.dispose();
        this.states.delete(key);
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
        vscode.window.setStatusBarMessage(
            `cflat: line ${sourceLine} produced no ${state.kind === 'ir' ? 'IR' : 'assembly'}`
            + ` - showing the nearest emitted line ${nearest}`, 5000);
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

    dispose(): void {
        for (const state of this.states.values()) {
            state.decoration.dispose();
            state.outerDecoration.dispose();
        }
        this.changeEmitter.dispose();
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
        vscode.window.showWarningMessage(
            'cflat: could not locate the cflat compiler (cflat.exe on Windows, cflat on macOS/Linux). ' +
            'Run "cflat --init" to record the compiler path, ' +
            'or set cflat.executablePath in Settings to enable the language server.'
        );
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
            vscode.window.showErrorMessage(
                'cflat: no .cb / .c source file to debug. ' +
                'Open the source file in the active editor, or set "program" in launch.json to a .cb path.'
            );
            return undefined;
        }

        const cflatExe = findCompilerExecutable();
        if (!cflatExe) {
            vscode.window.showErrorMessage(
                'cflat: cflat.executablePath is not set. Set it in Settings to enable debugging.'
            );
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
        { location: vscode.ProgressLocation.Window, title: `cflat: compiling ${path.basename(source)}...` },
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
                vscode.window.showErrorMessage(`cflat: failed to launch compiler (${err.message}).`);
                resolve(false);
            });
            proc.on('close', code => {
                if (code === 0) {
                    outputChannel.appendLine('Compile OK.');
                    resolve(true);
                } else {
                    outputChannel.appendLine(`Compile failed (exit ${code}).`);
                    outputChannel.show(true);
                    vscode.window.showErrorMessage('cflat: compile failed. See "cflat Language Server" output for details.');
                    resolve(false);
                }
            });
        })
    );
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

    const sourceLine = editor.selection.active.line + 1;
    const viewUri = provider.createView(editor.document.uri.toString(), kind, choice);
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

    void startClient();

    const viewProvider = new CflatViewContentProvider(() => client);
    context.subscriptions.push(
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
        vscode.window.setStatusBarMessage('cflat Language Server started', 3000)
    );
}

export function deactivate(): Thenable<void> | undefined {
    if (!client) {
        return undefined;
    }
    return client.stop();
}
