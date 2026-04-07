import * as path from 'path';
import * as vscode from 'vscode';
import {
    LanguageClient,
    LanguageClientOptions,
    ServerOptions,
    TransportKind
} from 'vscode-languageclient/node';

let client: LanguageClient;

export function activate(context: vscode.ExtensionContext): void {
    const serverModule = context.asAbsolutePath(path.join('out', 'server.js'));

    const serverOptions: ServerOptions = {
        run: {
            module: serverModule,
            transport: TransportKind.ipc
        },
        debug: {
            module: serverModule,
            transport: TransportKind.ipc,
            options: { execArgv: ['--nolazy', '--inspect=6009'] }
        }
    };

    const clientOptions: LanguageClientOptions = {
        documentSelector: [{ scheme: 'file', language: 'cflat' }],
        synchronize: {
            fileEvents: vscode.workspace.createFileSystemWatcher('**/*.{cb,c}')
        },
        outputChannelName: 'MyCompiler Language Server'
    };

    client = new LanguageClient(
        'mycompilerLanguageServer',
        'MyCompiler Language Server',
        serverOptions,
        clientOptions
    );

    client.start();

    // Command: manually trigger diagnostics on current file
    context.subscriptions.push(
        vscode.commands.registerCommand('mycompiler.runDiagnostics', () => {
            const editor = vscode.window.activeTextEditor;
            if (editor && editor.document.languageId === 'myc') {
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
