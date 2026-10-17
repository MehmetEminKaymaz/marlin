#ifndef MARLINLSP_H
#define MARLINLSP_H

#include <string>

// ---------------------------------------------------------------------------
// marlin dil sunucusu (Language Server Protocol)
//
//   marlin_lang --lsp
//
// Editörle (VS Code eklentisi: editors/vscode) standart girdi / çıktı
// üzerinden JSON-RPC ile konuşur. Derleyicinin kendi ön ucunu (ModuleLoader +
// Sema) kullanır; böylece editörün gösterdiği hatalar derleyicininkilerle
// birebir aynıdır. MLIR ve LLVM aşamaları çalışmaz.
//
// Şimdilik: açık belgelerdeki sözdizimi ve anlam hataları (yazdıkça).
// ---------------------------------------------------------------------------
namespace marlin {

/// `compilerPath`: çalışan derleyicinin yolu (std dizinini bulmak için).
int runLanguageServer(const std::string& compilerPath);

} // namespace marlin

#endif // MARLINLSP_H
