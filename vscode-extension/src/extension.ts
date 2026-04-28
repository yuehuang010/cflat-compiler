import * as fs from 'fs';
import * as path from 'path';
import * as vscode from 'vscode';
import {
    LanguageClient,
    LanguageClientOptions,
    ServerOptions,
    TransportKind
} from 'vscode-languageclient/node';

let client: LanguageClient;

function findCompilerExecutable(): string | undefined {
    // 1. Explicit setting always wins.
    const configured = vscode.workspace.getConfiguration('mycompiler').get<string>('executablePath');
    if (configured && configured.trim() !== '') {
        return configured.trim();
    }

    // 2. Scan common build-output locations relative to each workspace folder.
    const candidates = [
        path.join('x64', 'Debug',   'MyCompiler.exe'),
        path.join('x64', 'Release', 'MyCompiler.exe'),
        path.join('x86', 'Debug',   'MyCompiler.exe'),
        path.join('x86', 'Release', 'MyCompiler.exe'),
    ];
    for (const folder of vscode.workspace.workspaceFolders ?? []) {
        for (const rel of candidates) {
            const full = path.join(folder.uri.fsPath, rel);
            if (fs.existsSync(full)) {
                return full;
            }
        }
    }

    return undefined;
}

export function activate(context: vscode.ExtensionContext): void {
    const outputChannel = vscode.window.createOutputChannel('MyCompiler Language Server');
    context.subscriptions.push(outputChannel);

    outputChannel.appendLine('=== MyCompiler Extension Activating ===');
    outputChannel.appendLine(`Extension path : ${context.extensionPath}`);
    outputChannel.appendLine(`Log directory  : ${context.logUri.fsPath}`);

    const workspaceFolders = vscode.workspace.workspaceFolders ?? [];
    if (workspaceFolders.length === 0) {
        outputChannel.appendLine('Workspace folders: (none)');
    } else {
        workspaceFolders.forEach(f => outputChannel.appendLine(`Workspace folder: ${f.uri.fsPath}`));
    }

    const configured = vscode.workspace.getConfiguration('mycompiler').get<string>('executablePath');
    outputChannel.appendLine(`mycompiler.executablePath setting: "${configured ?? ''}"`);

    const exePath = findCompilerExecutable();
    if (!exePath) {
        outputChannel.appendLine('ERROR: MyCompiler.exe not found — checked x64/Debug, x64/Release, x86/Debug, x86/Release relative to each workspace folder.');
        outputChannel.show(true);
        vscode.window.showWarningMessage(
            'MyCompiler: could not find MyCompiler.exe. ' +
            'Set mycompiler.executablePath in settings or build the project first.'
        );
        return;
    }

    outputChannel.appendLine(`Compiler exe   : ${exePath}`);

    const coreDir = path.join(path.dirname(exePath), 'core');
    const runtimeCb = path.join(coreDir, 'runtime.cb');
    outputChannel.appendLine(`Core directory : ${coreDir} (${fs.existsSync(coreDir) ? 'exists' : 'MISSING'})`);
    outputChannel.appendLine(`runtime.cb     : ${runtimeCb} (${fs.existsSync(runtimeCb) ? 'exists' : 'MISSING'})`);

    const logPath = path.join(context.logUri.fsPath, 'lsp.log');

    const serverOptions: ServerOptions = {
        run:   { command: exePath, args: ['lsp'],                                      transport: TransportKind.stdio },
        debug: { command: exePath, args: ['lsp', '--verbose', '--log-file', logPath],  transport: TransportKind.stdio }
    };

    const clientOptions: LanguageClientOptions = {
        documentSelector: [{ scheme: 'file', language: 'cflat' }],
        synchronize: {
            fileEvents: vscode.workspace.createFileSystemWatcher('**/*.{cb,c}')
        },
        outputChannel
    };

    client = new LanguageClient(
        'mycompilerLanguageServer',
        'MyCompiler Language Server',
        serverOptions,
        clientOptions
    );

    outputChannel.appendLine('Starting LSP client...');
    client.start();
    outputChannel.appendLine('LSP client started.');

    // Command: manually trigger diagnostics on current file
    context.subscriptions.push(
        vscode.commands.registerCommand('mycompiler.runDiagnostics', () => {
            const editor = vscode.window.activeTextEditor;
            if (editor && editor.document.languageId === 'cflat') {
                client.sendNotification('mycompiler/runDiagnostics', {
                    uri: editor.document.uri.toString()
                });
            }
        })
    );

    // Command: show compiler output channel
    context.subscriptions.push(
        vscode.commands.registerCommand('mycompiler.showOutput', () => {
            client.outputChannel.show();
        })
    );

    context.subscriptions.push(
        vscode.window.setStatusBarMessage('MyCompiler Language Server started', 3000)
    );
}

export function deactivate(): Thenable<void> | undefined {
    if (!client) {
        return undefined;
    }
    return client.stop();
}
