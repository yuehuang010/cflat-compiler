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
let outputChannel: vscode.OutputChannel;
let logFilePath: string;

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

    const clientOptions: LanguageClientOptions = {
        documentSelector: [{ scheme: 'file', language: 'cflat' }],
        synchronize: {
            fileEvents: vscode.workspace.createFileSystemWatcher('**/*.{cb,c}')
        },
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

export function activate(context: vscode.ExtensionContext): void {
    outputChannel = vscode.window.createOutputChannel('cflat Language Server');
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
