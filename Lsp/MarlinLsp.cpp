// marlin dil sunucusu (bkz. MarlinLsp.h)
//
// JSON-RPC bağlantısı ve LSP mesaj tipleri MLIR'in kendi dil sunucularının
// (mlir-lsp-server, tblgen-lsp-server) kullandığı kütüphanedendir
// (mlir/Tools/lsp-server-support); burada yalnızca marlin'e özgü kısım vardır.
//
// AKIŞ
//
//   didOpen / didChange   belgenin metni güncellenir (tam metin), sonra
//                         belge yeniden denetlenir:
//                           ModuleLoader (import'larla birlikte; açık
//                           belgeler diskten değil editörden okunur) + Sema
//   publishDiagnostics    hatalar dosya dosya gönderilir. Bir dosyada hata
//                         kalmadıysa boş liste gönderilir (editör temizler).
//
// Hata metinleri derleyicininkilerle aynıdır: "yol:satir:sutun: hata: mesaj".
// Burada geri ayrıştırılıp LSP tanılamasına çevrilir.

#include "MarlinLsp.h"
#include "MarlinCheck.h"

#include "mlir/Tools/lsp-server-support/Logging.h"
#include "mlir/Tools/lsp-server-support/Protocol.h"
#include "mlir/Tools/lsp-server-support/Transport.h"
#include "llvm/Support/raw_ostream.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <sstream>

namespace lsp = mlir::lsp;
namespace fs = std::filesystem;

namespace marlin {
namespace {

std::string canonical(const std::string& path) {
    std::error_code ec;
    return fs::weakly_canonical(path, ec).string();
}

bool readFile(const std::string& path, std::string& out) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    std::stringstream ss;
    ss << in.rdbuf();
    out = ss.str();
    return true;
}

// "yol:satir:sutun: hata: mesaj" (satır ve sütun 1'den başlar). Konumu
// olmayan mesajlar (ör. "programda main yok") belgenin başına konur.
struct ParsedError {
    std::string path;
    int line = 1, column = 1;
    std::string message;
};

ParsedError parseError(const std::string& text, const std::string& fallbackPath) {
    ParsedError result{fallbackPath, 1, 1, text};
    const size_t marker = text.find(": hata: ");
    if (marker == std::string::npos) return result;
    result.message = text.substr(marker + 8);
    const std::string head = text.substr(0, marker);

    auto isNumber = [](const std::string& s) {
        return !s.empty() && std::all_of(s.begin(), s.end(), [](unsigned char c) { return std::isdigit(c); });
    };
    const size_t second = head.rfind(':');
    const size_t first = second == std::string::npos || second == 0 ? std::string::npos
                                                                    : head.rfind(':', second - 1);
    if (first != std::string::npos && isNumber(head.substr(first + 1, second - first - 1)) &&
        isNumber(head.substr(second + 1))) {
        result.path = head.substr(0, first);
        result.line = std::stoi(head.substr(first + 1, second - first - 1));
        result.column = std::stoi(head.substr(second + 1));
    } else {
        result.path = head; // yalnızca dosya adı
    }
    return result;
}

// Hatanın vurgulanacağı aralık: başlangıçtaki kelime (isim, sayı, noktalı
// isim). Sütunlar karakter (kod noktası) sayar; Türkçe harfler tek birimdir.
lsp::Range rangeAt(const std::string& text, int line, int column) {
    const int zeroLine = std::max(0, line - 1);
    const int start = std::max(0, column - 1);

    // Satırın baytları.
    size_t lineBegin = 0;
    for (int l = 0; l < zeroLine && lineBegin != std::string::npos; ++l) {
        lineBegin = text.find('\n', lineBegin);
        if (lineBegin != std::string::npos) ++lineBegin;
    }
    if (lineBegin == std::string::npos) lineBegin = text.size();
    size_t lineEnd = text.find('\n', lineBegin);
    if (lineEnd == std::string::npos) lineEnd = text.size();

    // `start` karakter ilerle (UTF-8: devam baytları sayılmaz).
    size_t at = lineBegin;
    for (int c = 0; c < start && at < lineEnd; ++c) {
        ++at;
        while (at < lineEnd && (static_cast<unsigned char>(text[at]) & 0xC0) == 0x80) ++at;
    }
    int length = 0;
    while (at < lineEnd) {
        const unsigned char c = static_cast<unsigned char>(text[at]);
        const bool word = std::isalnum(c) || c == '_' || c == '.' || c >= 0x80;
        if (!word) break;
        ++at;
        while (at < lineEnd && (static_cast<unsigned char>(text[at]) & 0xC0) == 0x80) ++at;
        ++length;
    }
    if (length == 0) length = 1;
    return lsp::Range(lsp::Position(zeroLine, start), lsp::Position(zeroLine, start + length));
}

class Server {
public:
    Server(lsp::MessageHandler& handler, std::string stdDir)
        : stdDir(std::move(stdDir)),
          publishDiagnostics(handler.outgoingNotification<lsp::PublishDiagnosticsParams>(
              "textDocument/publishDiagnostics")) {
        handler.method("initialize", this, &Server::onInitialize);
        handler.notification("initialized", this, &Server::onInitialized);
        handler.method("shutdown", this, &Server::onShutdown);
        handler.notification("textDocument/didOpen", this, &Server::onOpen);
        handler.notification("textDocument/didChange", this, &Server::onChange);
        handler.notification("textDocument/didClose", this, &Server::onClose);
    }

private:
    struct Document {
        std::string text;
        int64_t version = 0;
    };

    std::string stdDir;
    std::map<std::string, Document> documents;                    // kesin yol → belge
    std::map<std::string, std::set<std::string>> publishedFiles;  // denetlenen kök → hata gönderilen dosyalar
    std::map<std::string, std::set<std::string>> imports;         // kök → yüklediği modül dosyaları
    lsp::OutgoingNotification<lsp::PublishDiagnosticsParams> publishDiagnostics;

    void onInitialize(const lsp::InitializeParams&, lsp::Callback<llvm::json::Value> reply) {
        llvm::json::Object sync{
            {"openClose", true},
            {"change", static_cast<int>(lsp::TextDocumentSyncKind::Full)},
        };
        reply(llvm::json::Object{
            {"serverInfo", llvm::json::Object{{"name", "marlin"}, {"version", "0.1"}}},
            {"capabilities", llvm::json::Object{{"textDocumentSync", std::move(sync)}}},
        });
    }

    void onInitialized(const lsp::InitializedParams&) {}

    void onShutdown(const lsp::NoParams&, lsp::Callback<std::nullptr_t> reply) { reply(nullptr); }

    void onOpen(const lsp::DidOpenTextDocumentParams& params) {
        const std::string path = canonical(params.textDocument.uri.file().str());
        documents[path] = {params.textDocument.text, params.textDocument.version};
        check(path);
    }

    void onChange(const lsp::DidChangeTextDocumentParams& params) {
        const std::string path = canonical(params.textDocument.uri.file().str());
        Document& doc = documents[path];
        if (mlir::failed(lsp::TextDocumentContentChangeEvent::applyTo(params.contentChanges, doc.text))) {
            lsp::Logger::error("belge degisikligi uygulanamadi: {0}", path);
            return;
        }
        doc.version = params.textDocument.version;
        check(path);
        // Bu dosyayı import eden açık belgelerin hataları da değişmiş olabilir.
        for (auto& entry : documents)
            if (entry.first != path && imports[entry.first].count(path)) check(entry.first);
    }

    void onClose(const lsp::DidCloseTextDocumentParams& params) {
        const std::string path = canonical(params.textDocument.uri.file().str());
        documents.erase(path);
        for (const std::string& file : publishedFiles[path]) publish(file, {});
        publishedFiles.erase(path);
        imports.erase(path);
        publish(path, {});
    }

    // Belgeyi (ve import ettiklerini) derleyicinin ön ucuyla denetler.
    void check(const std::string& root) {
        std::unordered_map<std::string, std::string> overlay;
        for (auto& [path, doc] : documents) overlay[path] = doc.text;

        CheckResult result = checkProgram(root, stdDir, overlay);
        const std::vector<std::string>& errors = result.errors;
        if (!result.modules.empty()) {
            std::set<std::string>& loaded = imports[root];
            loaded.clear();
            for (const std::string& module : result.modules) loaded.insert(canonical(module));
        }

        std::map<std::string, std::vector<lsp::Diagnostic>> byFile;
        byFile[root]; // hata yoksa da gönderilir: editör eskileri siler
        for (const std::string& message : errors) {
            ParsedError parsed = parseError(message, root);
            const std::string file = canonical(parsed.path);
            lsp::Diagnostic diagnostic;
            diagnostic.range = rangeAt(textOf(file), parsed.line, parsed.column);
            diagnostic.severity = lsp::DiagnosticSeverity::Error;
            diagnostic.source = "marlin";
            diagnostic.message = parsed.message;
            byFile[file].push_back(std::move(diagnostic));
        }

        std::set<std::string>& previous = publishedFiles[root];
        for (const std::string& file : previous)
            if (!byFile.count(file)) publish(file, {});
        previous.clear();
        for (auto& [file, diagnostics] : byFile) {
            previous.insert(file);
            publish(file, std::move(diagnostics));
        }
    }

    std::string textOf(const std::string& path) const {
        auto open = documents.find(path);
        if (open != documents.end()) return open->second.text;
        std::string text;
        readFile(path, text);
        return text;
    }

    void publish(const std::string& path, std::vector<lsp::Diagnostic> diagnostics) {
        llvm::Expected<lsp::URIForFile> uri = lsp::URIForFile::fromFile(path);
        if (!uri) {
            llvm::consumeError(uri.takeError());
            return;
        }
        auto open = documents.find(path);
        lsp::PublishDiagnosticsParams params(*uri, open != documents.end() ? open->second.version : 0);
        params.diagnostics = std::move(diagnostics);
        publishDiagnostics(params);
    }
};

} // namespace

int runLanguageServer(const std::string& compilerPath) {
    lsp::Logger::setLogLevel(lsp::Logger::Level::Error);
    lsp::JSONTransport transport(stdin, llvm::outs());
    lsp::MessageHandler handler(transport);
    Server server(handler, findStdDirectory(compilerPath));
    if (llvm::Error error = transport.run(handler)) {
        llvm::consumeError(std::move(error));
        return 1;
    }
    return 0;
}

} // namespace marlin
