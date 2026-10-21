'use strict';

const vscode = require('vscode');
const fs = require('fs');
const path = require('path');

const requestPattern = '.vscode/engine-attach-request.json';
const recentRequestMs = 60_000;
const codeExtensions = new Set([
    '.h', '.hpp', '.cpp', '.cc', '.cxx', '.inl', '.cs', '.c', '.m', '.mm',
    '.py', '.lua', '.js', '.ts', '.sh', '.ps1'
]);

function activate(context) {
    const lastRequest = new Map();
    const inFlight = new Set();
    const attached = new Set();

    function rememberEditor(editor) {
        if (!vscode.workspace.isTrusted || !editor) return;
        const document = editor.document;
        if (!document || document.uri.scheme !== 'file' ||
            !codeExtensions.has(path.extname(document.uri.fsPath).toLowerCase()))
            return;
        const folder = vscode.workspace.getWorkspaceFolder(document.uri);
        if (!folder) return;
        const record = path.join(folder.uri.fsPath, '.vscode',
            'engine-last-code-file.txt');
        fs.promises.writeFile(record, document.uri.fsPath + '\n', 'utf8')
            .catch(() => {});
    }

    async function check(folder) {
        if (!vscode.workspace.isTrusted) return;
        const requestPath = path.join(folder.uri.fsPath, '.vscode',
            'engine-attach-request.json');
        let request;
        try {
            request = JSON.parse(await fs.promises.readFile(requestPath, 'utf8'));
        } catch {
            return;
        }
        const { pid, requestedAt } = request;
        const age = Date.now() - requestedAt;
        if (!Number.isSafeInteger(pid) || pid <= 0 ||
            !Number.isSafeInteger(requestedAt) || age < -5_000 ||
            age > recentRequestMs) return;

        const key = `${folder.uri.fsPath.toLowerCase()}:${pid}`;
        if (lastRequest.get(key) === requestedAt || inFlight.has(key) ||
            attached.has(key)) return;
        lastRequest.set(key, requestedAt);

        const active = vscode.debug.activeDebugSession;
        if (active && active.type === 'cppvsdbg') return;

        const build = path.join(folder.uri.fsPath, 'build', 'Debug');
        const library = fs.existsSync(path.join(build, 'Engine'))
            ? path.join(build, 'Engine', 'lib', 'Debug')
            : path.join(build, 'lib', 'Debug');
        const configuration = {
            name: `Engine Editor auto attach (${pid})`,
            type: 'cppvsdbg',
            request: 'attach',
            processId: String(pid),
            engineAutoAttachKey: key,
            symbolOptions: {
                searchPaths: [library, path.join(build, '.hotreload')]
            }
        };

        inFlight.add(key);
        try {
            const started = await vscode.debug.startDebugging(folder, configuration);
            if (!started) {
                vscode.window.showWarningMessage(
                    `Could not attach the C++ debugger to Engine Editor ${pid}.`);
            }
        } catch (error) {
            vscode.window.showWarningMessage(
                `Could not attach to Engine Editor ${pid}: ${error.message}`);
        } finally {
            inFlight.delete(key);
        }
    }

    function watch(folder) {
        const watcher = vscode.workspace.createFileSystemWatcher(
            new vscode.RelativePattern(folder, requestPattern));
        const refresh = () => setTimeout(() => check(folder), 100);
        context.subscriptions.push(watcher, watcher.onDidCreate(refresh),
            watcher.onDidChange(refresh));
        check(folder);
    }

    context.subscriptions.push(
        vscode.window.onDidChangeActiveTextEditor(rememberEditor),
        vscode.debug.onDidStartDebugSession(session => {
            if (session.configuration.engineAutoAttachKey)
                attached.add(session.configuration.engineAutoAttachKey);
        }),
        vscode.debug.onDidTerminateDebugSession(session => {
            if (session.configuration.engineAutoAttachKey)
                attached.delete(session.configuration.engineAutoAttachKey);
        })
    );
    for (const folder of vscode.workspace.workspaceFolders || []) watch(folder);
    rememberEditor(vscode.window.activeTextEditor);
    context.subscriptions.push(vscode.workspace.onDidChangeWorkspaceFolders(event => {
        for (const folder of event.added) watch(folder);
    }));
}

function deactivate() {}

module.exports = { activate, deactivate };
