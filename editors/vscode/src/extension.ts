// marlin VS Code eklentisi
//
//   * Renklendirme, parantez / yorum davranışı ve snippet'ler package.json'da
//     tanımlıdır (syntaxes/, language-configuration.json, snippets/); kod
//     gerektirmez.
//   * Dil sunucusu: derleyicinin kendisi (`marlin --lsp`). Açık .marl
//     belgelerini yazdıkça derleyicinin ön ucuyla denetler ve hataları
//     gösterir (bkz. Lsp/MarlinLsp.cpp).
//   * Komutlar: Derle ve Çalıştır, Testleri Çalıştır, Dil Sunucusunu Yeniden Başlat.
//
// Eklenti çalışma alanı tarafında çalışır (extensionKind: workspace): VS Code
// WSL'e bağlıyken eklenti de WSL'de çalışır ve derleyiciyi doğrudan başlatır.

import * as fs from "fs";
import * as os from "os";
import * as path from "path";
import * as vscode from "vscode";
import { LanguageClient, LanguageClientOptions, ServerOptions } from "vscode-languageclient/node";

let client: LanguageClient | undefined;
let terminal: vscode.Terminal | undefined;

// Derleyicinin yolu: ayar, yoksa çalışma alanındaki derleme dizini, yoksa PATH.
function compilerPath(): string {
    const configured = vscode.workspace.getConfiguration("marlin").get<string>("compilerPath");
    if (configured) return configured;
    for (const folder of vscode.workspace.workspaceFolders ?? []) {
        const candidate = path.join(folder.uri.fsPath, "cmake-build-debug", "marlin");
        if (fs.existsSync(candidate)) return candidate;
    }
    return "marlin";
}

// Kabukta güvenli tırnaklama (bash): 'a b' → 'a b', a'b → 'a'\''b'
function quote(text: string): string {
    return "'" + text.replace(/'/g, "'\\''") + "'";
}

async function startServer(context: vscode.ExtensionContext): Promise<void> {
    if (!vscode.workspace.getConfiguration("marlin").get<boolean>("languageServer", true)) return;
    const command = compilerPath();
    const serverOptions: ServerOptions = { command, args: ["--lsp"] };
    const clientOptions: LanguageClientOptions = {
        documentSelector: [{ scheme: "file", language: "marlin" }],
        outputChannelName: "Marlin",
    };
    client = new LanguageClient("marlin", "Marlin", serverOptions, clientOptions);
    try {
        await client.start();
    } catch (error) {
        client = undefined;
        vscode.window.showErrorMessage(
            `marlin dil sunucusu başlatılamadı (${command} --lsp). Derleyiciyi derlediğinizden ` +
            `emin olun ya da 'marlin.compilerPath' ayarını verin. Ayrıntı: ${error}`);
    }
}

async function stopServer(): Promise<void> {
    if (!client) return;
    const running = client;
    client = undefined;
    await running.stop();
}

function marlinTerminal(): vscode.Terminal {
    if (!terminal || terminal.exitStatus !== undefined) terminal = vscode.window.createTerminal("Marlin");
    terminal.show(true);
    return terminal;
}

// Açık dosyayı kaydeder, derler ve çalıştırır. Çalıştırılabilir dosya geçici
// dizine yazılır (proje dizini kirlenmesin).
async function runCurrentFile(): Promise<void> {
    const editor = vscode.window.activeTextEditor;
    if (!editor || editor.document.languageId !== "marlin") {
        vscode.window.showWarningMessage("Açık bir .marl dosyası yok.");
        return;
    }
    if (editor.document.isDirty) await editor.document.save();

    const source = editor.document.uri.fsPath;
    const program = path.join(os.tmpdir(), "marlin-" + path.basename(source, ".marl"));
    const args = vscode.workspace.getConfiguration("marlin").get<string[]>("runArgs", []);
    const compile = [compilerPath(), source, ...args, "-o", program].map(quote).join(" ");
    const term = marlinTerminal();
    term.sendText(`cd ${quote(path.dirname(source))} && ${compile} && ${quote(program)}`);
}

// Çalışma alanındaki tests/run.sh.
function runTests(): void {
    const folder = vscode.workspace.workspaceFolders?.find((f) =>
        fs.existsSync(path.join(f.uri.fsPath, "tests", "run.sh")));
    if (!folder) {
        vscode.window.showWarningMessage("Çalışma alanında tests/run.sh bulunamadı.");
        return;
    }
    marlinTerminal().sendText(`cd ${quote(path.join(folder.uri.fsPath, "tests"))} && bash run.sh`);
}

export async function activate(context: vscode.ExtensionContext): Promise<void> {
    context.subscriptions.push(
        vscode.commands.registerCommand("marlin.run", runCurrentFile),
        vscode.commands.registerCommand("marlin.runTests", runTests),
        vscode.commands.registerCommand("marlin.restartServer", async () => {
            await stopServer();
            await startServer(context);
            if (client) vscode.window.setStatusBarMessage("marlin dil sunucusu yeniden başlatıldı", 3000);
        }),
        vscode.workspace.onDidChangeConfiguration(async (event) => {
            if (event.affectsConfiguration("marlin.compilerPath") ||
                event.affectsConfiguration("marlin.languageServer")) {
                await stopServer();
                await startServer(context);
            }
        }),
        vscode.window.onDidCloseTerminal((closed) => {
            if (closed === terminal) terminal = undefined;
        }),
    );
    await startServer(context);
}

export async function deactivate(): Promise<void> {
    await stopServer();
}
