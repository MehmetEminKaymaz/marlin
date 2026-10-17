#ifndef MARLINCHECK_H
#define MARLINCHECK_H

#include <string>
#include <unordered_map>
#include <vector>

// Dil sunucusunun derleyici ön ucuna açılan kapısı.
//
// Ayrı bir dosyadadır çünkü iki dünya bir arada derlenemez: AST ve Sema RTTI
// ile (dynamic_cast) derlenir, MLIR'in LSP kütüphanesini kullanan kod ise
// LLVM gibi RTTI olmadan (bkz. CMakeLists.txt). Aradaki arayüz yalnızca
// standart tiplerdir.
namespace marlin {

struct CheckResult {
    std::vector<std::string> errors;   // "yol:satir:sutun: hata: mesaj" (derleyicininkiyle aynı)
    std::vector<std::string> modules;  // yüklenen modül dosyaları (kök dahil); yükleme başarısızsa boş
};

/// `root` dosyasını import'larıyla birlikte ayrıştırır ve Sema'dan geçirir.
/// `overlay`: editörde açık belgeler (kesin yol → metin); diskten önce bakılır.
CheckResult checkProgram(const std::string& root, const std::string& stdDir,
                         const std::unordered_map<std::string, std::string>& overlay);

/// Derleyiciyle gelen std dizini (bkz. ModuleLoader::findStdDir).
std::string findStdDirectory(const std::string& compilerPath);

} // namespace marlin

#endif // MARLINCHECK_H
