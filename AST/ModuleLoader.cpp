#include "ModuleLoader.h"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>

#include "antlr4-runtime.h"
#include "minikLexer.h"
#include "minikParser.h"
#include "ASTBuilder.h"

namespace fs = std::filesystem;

namespace {

// ANTLR'ın sözdizimi hatalarını, diğer hatalarla aynı biçimde
// (dosya:satır:sütun) toplar.
class SyntaxErrorCollector : public antlr4::BaseErrorListener {
public:
    SyntaxErrorCollector(const std::string& path, std::vector<std::string>& errors)
        : path(path), errors(errors) {}

    void syntaxError(antlr4::Recognizer*, antlr4::Token*, size_t line, size_t charPositionInLine,
                     const std::string& msg, std::exception_ptr) override {
        errors.push_back(path + ":" + std::to_string(line) + ":" +
                         std::to_string(charPositionInLine + 1) + ": hata: sozdizimi hatasi: " + msg);
        ++count;
    }

    int count = 0;

private:
    const std::string& path;
    std::vector<std::string>& errors;
};

bool readFile(const std::string& path, std::string& out) {
    std::ifstream file(path);
    if (!file) return false;
    std::stringstream buffer;
    buffer << file.rdbuf();
    out = buffer.str();
    return true;
}

bool isStandardModule(const std::string& name) {
    return name == "std" || name.rfind("std.", 0) == 0;
}

// extern fonksiyonun C'deki adı: modül ve fonksiyon adından türetilir.
//   std.file   / c_exists  →  minik_file_exists
//   std.net.ip / c_parse   →  minik_net_ip_parse
// (`c_` öneki, extern'i aynı dosyadaki sarmalayıcısından ayırmak içindir.)
std::string externSymbol(const std::string& moduleName, const std::string& function) {
    std::string module = isStandardModule(moduleName) && moduleName.size() > 4
                             ? moduleName.substr(4) : moduleName;
    std::replace(module.begin(), module.end(), '.', '_');
    const std::string base = function.rfind("c_", 0) == 0 ? function.substr(2) : function;
    return "minik_" + module + "_" + base;
}

} // namespace

std::string ModuleLoader::findStdDir(const std::string& compilerPath) {
    std::error_code ec;
    if (const char* fromEnv = std::getenv("MINIK_STD"))
        return fs::is_directory(fromEnv, ec) ? fromEnv : "";

    const fs::path beside = fs::path(compilerPath).parent_path() / "std";
    if (fs::is_directory(beside, ec)) return beside.string();

#ifdef MINIK_STD_DIR
    if (fs::is_directory(MINIK_STD_DIR, ec)) return MINIK_STD_DIR;
#endif
    return "";
}

void ModuleLoader::error(int module, const ASTNode* at, const std::string& msg) {
    errors.push_back(program->modules[module].path + ":" + std::to_string(at->line) + ":" +
                     std::to_string(at->column) + ": hata: " + msg);
}

std::unique_ptr<Program> ModuleLoader::load(const std::string& rootPath) {
    std::string source;
    if (!readFile(rootPath, source)) {
        errors.push_back("Dosya acilamadi: " + rootPath);
        return nullptr;
    }
    program = std::make_unique<Program>();
    canonicalPaths.clear();
    addModule("", rootPath, source);
    return finish();
}

std::unique_ptr<Program> ModuleLoader::loadSource(const std::string& source,
                                                  const std::string& displayName) {
    program = std::make_unique<Program>();
    canonicalPaths.clear();
    addModule("", displayName, source);
    canonicalPaths[0].clear(); // dosyası yok; hiçbir import onunla eşleşmez
    return finish();
}

// Yüklenen modüllerin import'larını izler. Liste, yeni modüller eklendikçe
// büyür; hepsi işlenince biter.
std::unique_ptr<Program> ModuleLoader::finish() {
    for (size_t m = 0; m < program->modules.size(); ++m)
        for (size_t i = 0; i < program->modules[m].imports.size(); ++i)
            resolveImport(static_cast<int>(m), program->modules[m].imports[i].get());

    if (!errors.empty()) return nullptr;
    return std::move(program);
}

// "araclar.vektor" → "araclar$vektor$". Farklı dizinlerdeki iki dosya aynı
// yolla import edilmişse önekler bir sayıyla ayrılır.
std::string ModuleLoader::makePrefix(const std::string& name) const {
    if (name.empty()) return "";
    std::string base = name;
    std::replace(base.begin(), base.end(), '.', '$');

    std::string prefix = base + "$";
    for (int n = 2;; ++n) {
        bool taken = false;
        for (const Module& m : program->modules) taken = taken || m.prefix == prefix;
        if (!taken) return prefix;
        prefix = base + "$" + std::to_string(n) + "$";
    }
}

// Dosyayı ayrıştırır ve bildirimlerini programa ekler. Modülün sırasını döndürür.
int ModuleLoader::addModule(const std::string& name, const std::string& path,
                            const std::string& source) {
    const int index = static_cast<int>(program->modules.size());
    const std::string prefix = makePrefix(name);
    program->modules.push_back({name, path, prefix, {}});
    std::error_code ec;
    canonicalPaths.push_back(fs::weakly_canonical(path, ec).string());

    antlr4::ANTLRInputStream input(source);
    minikLexer lexer(&input);
    antlr4::CommonTokenStream tokens(&lexer);
    minikParser parser(&tokens);

    SyntaxErrorCollector collector(path, errors);
    lexer.removeErrorListeners();
    parser.removeErrorListeners();
    lexer.addErrorListener(&collector);
    parser.addErrorListener(&collector);

    minikParser::ProgramContext* tree = parser.program();
    if (collector.count > 0) return index;

    ASTBuilder builder(index);
    std::any result = builder.visit(tree);
    if (!result.has_value()) return index;
    std::unique_ptr<Program> file(std::any_cast<Program*>(result));

    program->modules[index].imports = std::move(file->modules[0].imports);
    for (auto& decl : file->declarations) {
        if (auto* fn = dynamic_cast<FunctionDecl*>(decl.get())) {
            if (fn->isExtern) fn->externName = externSymbol(name, fn->name);
            fn->name = prefix + fn->name;
        }
        else if (auto* var = dynamic_cast<VariableDecl*>(decl.get())) var->name = prefix + var->name;
        else if (auto* st = dynamic_cast<StructDecl*>(decl.get())) st->name = prefix + st->name;
        program->declarations.push_back(std::move(decl));
    }
    return index;
}

void ModuleLoader::resolveImport(int importer, ImportDecl* decl) {
    const std::string& name = decl->modulePath;

    // Standart modül yalnızca std dizininde aranır: std.file → <std>/file.minik.
    // Kullanıcının dizinlerine bakılmaz; böylece `std` adı taklit edilemez.
    const bool standard = isStandardModule(name);
    std::string relative = standard ? (name.size() > 4 ? name.substr(4) : "") : name;
    std::replace(relative.begin(), relative.end(), '.', '/');
    relative += ".minik";

    std::vector<fs::path> dirs;
    if (standard) {
        if (!stdDir.empty()) dirs.push_back(stdDir);
    } else {
        dirs.push_back(fs::path(program->modules[importer].path).parent_path());
        dirs.insert(dirs.end(), searchDirs.begin(), searchDirs.end());
    }

    fs::path found;
    std::error_code ec;
    for (const fs::path& dir : dirs) {
        fs::path candidate = (dir / relative).lexically_normal();
        if (fs::is_regular_file(candidate, ec)) { found = candidate; break; }
    }
    if (found.empty()) {
        if (standard)
            error(importer, decl, "'" + name + "' adinda bir standart modul yok");
        else
            error(importer, decl, "modul bulunamadi: '" + name + "' (aranan dosya: " + relative + ")");
        return;
    }

    // Aynı dosya daha önce yüklendiyse (başka bir yoldan ya da döngüyle) o kullanılır.
    const std::string canonical = fs::weakly_canonical(found, ec).string();
    for (size_t m = 0; m < canonicalPaths.size(); ++m) {
        if (!canonical.empty() && canonicalPaths[m] == canonical) {
            decl->target = static_cast<int>(m);
            return;
        }
    }

    std::string source;
    if (!readFile(found.string(), source)) {
        error(importer, decl, "modul dosyasi acilamadi: " + found.string());
        return;
    }
    decl->target = addModule(name, found.string(), source);
}
