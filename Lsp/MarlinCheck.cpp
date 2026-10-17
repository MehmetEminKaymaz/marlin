// Dil sunucusu için derleyici ön ucu (bkz. MarlinCheck.h).

#include "MarlinCheck.h"

#include "../AST/ModuleLoader.h"
#include "../AST/Sema.h"

namespace marlin {

CheckResult checkProgram(const std::string& root, const std::string& stdDir,
                         const std::unordered_map<std::string, std::string>& overlay) {
    CheckResult result;
    ModuleLoader loader({}, stdDir);
    loader.setOverlay(&overlay);
    std::unique_ptr<Program> program = loader.load(root);
    if (!program) {
        result.errors = loader.getErrors();
        return result;
    }
    for (const Module& module : program->modules) result.modules.push_back(module.path);

    Sema sema;
    sema.setRequireMain(false); // editörde açılan bir modül dosyasının main'i olmaz
    program->accept(sema);
    result.errors = sema.getErrors();
    return result;
}

std::string findStdDirectory(const std::string& compilerPath) {
    return ModuleLoader::findStdDir(compilerPath);
}

} // namespace marlin
