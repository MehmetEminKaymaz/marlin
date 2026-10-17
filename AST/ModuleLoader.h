#ifndef MODULELOADER_H
#define MODULELOADER_H

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>
#include "ASTNode.h"

// ---------------------------------------------------------------------------
// ModuleLoader — kaynak dosyaları yükler ve tek bir Program'da birleştirir
//
// Derlenen dosyadan (kök modül) başlar, `import` / `from` bildirimlerini
// izleyerek gereken bütün dosyaları ayrıştırır. Her dosya bir modüldür ve bir
// kez yüklenir; birbirini import eden dosyalar (döngü) sorun değildir, çünkü
// bildirimlerin sırası önemsizdir ve modüllerin çalıştırılan bir başlangıç
// kodu yoktur.
//
// Modül yolu noktalıdır: `araclar.vektor`, `araclar/vektor.marl` dosyasıdır.
// Dosya sırasıyla şuralarda aranır:
//   1. import eden dosyanın dizini
//   2. `-I` ile verilen dizinler
// `std` ile başlayan yollar standart kütüphaneye ayrılmıştır: yalnızca
// derleyiciyle gelen std dizininde aranır (`std.file` → <std>/file.marl).
//
// Sonuçta bütün bildirimler tek bir Program'dadır. Kök dışındaki modüllerin
// fonksiyon ve global adlarına modülün öneki eklenir (bkz. Module::prefix);
// isimlerin hangi modülden görüldüğünü Sema çözer.
// ---------------------------------------------------------------------------
class ModuleLoader {
public:
    // stdDir: standart kütüphane dosyalarının dizini (boşsa std modülleri bulunamaz).
    explicit ModuleLoader(std::vector<std::string> searchDirs = {}, std::string stdDir = "")
        : searchDirs(std::move(searchDirs)), stdDir(std::move(stdDir)) {}

    // Derleyiciyle gelen std dizinini bulur: MARLIN_STD ortam değişkeni,
    // derleyicinin yanındaki `std`, sonra derleme sırasında gömülen yol.
    static std::string findStdDir(const std::string& compilerPath);

    // Kök dosyayı ve import ettiklerini yükler. Hata varsa nullptr döner.
    std::unique_ptr<Program> load(const std::string& rootPath);
    // Kök modülün kaynağı dosyadan değil bellekten gelir (gömülü örnek program).
    std::unique_ptr<Program> loadSource(const std::string& source, const std::string& displayName);

    const std::vector<std::string>& getErrors() const { return errors; }

    // Editörde açık (kaydedilmemiş olabilecek) dosyalar: kesin yol → metin.
    // Bir dosya burada varsa diskten okunmaz (bkz. Lsp/MarlinLsp.cpp).
    void setOverlay(const std::unordered_map<std::string, std::string>* documents) {
        overlay = documents;
    }

private:
    std::vector<std::string> searchDirs;
    std::string stdDir;
    const std::unordered_map<std::string, std::string>* overlay = nullptr;
    bool readSource(const std::string& path, std::string& out) const;
    std::vector<std::string> errors;
    std::vector<std::string> canonicalPaths; // modül sırası → dosyanın kesin yolu
    std::unique_ptr<Program> program;

    std::unique_ptr<Program> finish();
    int addModule(const std::string& name, const std::string& path, const std::string& source);
    void resolveImport(int importer, ImportDecl* decl);
    std::string makePrefix(const std::string& name) const;
    void error(int module, const ASTNode* at, const std::string& msg);
};

#endif //MODULELOADER_H
