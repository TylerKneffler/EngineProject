'use strict';

const assert = require('assert');
const fs = require('fs');
const path = require('path');
const Module = require('module');

const workspace = path.resolve(__dirname, '..', 'test-output', 'VSCodeBridge');
const vscodeDirectory = path.join(workspace, '.vscode');
fs.mkdirSync(vscodeDirectory, { recursive: true });
const requestPath = path.join(vscodeDirectory, 'engine-attach-request.json');

let changed;
let activeEditorChanged;
let watcherPattern;
const starts = [];
const folder = { uri: { fsPath: workspace } };
const vscode = {
    workspace: {
        isTrusted: true,
        workspaceFolders: [folder],
        getWorkspaceFolder(uri) {
            return uri.fsPath.startsWith(workspace + path.sep) ? folder : undefined;
        },
        createFileSystemWatcher(pattern) {
            watcherPattern = pattern.pattern;
            return {
                onDidCreate(callback) { return { dispose() {} }; },
                onDidChange(callback) {
                    changed = callback;
                    return { dispose() {} };
                },
                dispose() {}
            };
        },
        onDidChangeWorkspaceFolders() { return { dispose() {} }; }
    },
    RelativePattern: class {
        constructor(folder, pattern) { this.pattern = pattern; }
    },
    debug: {
        activeDebugSession: null,
        async startDebugging(workspaceFolder, configuration) {
            starts.push({ workspaceFolder, configuration });
            return true;
        },
        onDidStartDebugSession() { return { dispose() {} }; },
        onDidTerminateDebugSession() { return { dispose() {} }; }
    },
    window: {
        activeTextEditor: undefined,
        onDidChangeActiveTextEditor(callback) {
            activeEditorChanged = callback;
            return { dispose() {} };
        },
        showWarningMessage(message) { throw new Error(message); }
    }
};

const originalLoad = Module._load;
Module._load = function (request, parent, isMain) {
    if (request === 'vscode') return vscode;
    return originalLoad.call(this, request, parent, isMain);
};
const bridge = require('../../Engine/Editor/VSCodeBridge/extension.js');
Module._load = originalLoad;

const delay = ms => new Promise(resolve => setTimeout(resolve, ms));
async function main() {
    const now = Date.now();
    fs.writeFileSync(requestPath, JSON.stringify({ pid: 12345, requestedAt: now }));
    bridge.activate({ subscriptions: [] });
    assert.strictEqual(watcherPattern, '.vscode/engine-attach-request.json');
    await delay(30);
    assert.strictEqual(starts.length, 1);
    assert.strictEqual(starts[0].workspaceFolder, folder);
    assert.strictEqual(starts[0].configuration.processId, '12345');
    assert.strictEqual(starts[0].configuration.type, 'cppvsdbg');

    const script = path.join(workspace, 'Scripts', 'Example.cpp');
    activeEditorChanged({ document: { uri: { scheme: 'file', fsPath: script } } });
    await delay(30);
    assert.strictEqual(fs.readFileSync(path.join(vscodeDirectory,
        'engine-last-code-file.txt'), 'utf8').trim(), script);

    activeEditorChanged({ document: { uri: { scheme: 'file',
        fsPath: path.join(workspace, 'Assets', 'Texture.png') } } });
    await delay(30);
    assert.strictEqual(fs.readFileSync(path.join(vscodeDirectory,
        'engine-last-code-file.txt'), 'utf8').trim(), script);

    changed();
    await delay(130);
    assert.strictEqual(starts.length, 1, 'duplicate requests must not attach twice');

    fs.writeFileSync(requestPath, JSON.stringify({ pid: 12345,
        requestedAt: now + 1 }));
    changed();
    await delay(130);
    assert.strictEqual(starts.length, 2, 'a new Play requests attach again');

    fs.writeFileSync(requestPath, JSON.stringify({ pid: 12345,
        requestedAt: now - 120000 }));
    changed();
    await delay(130);
    assert.strictEqual(starts.length, 2, 'stale requests must be ignored');
}

main().catch(error => { console.error(error); process.exitCode = 1; });
