# Marlin — VS Code eklentisi

marlin dili (`.marl`) için:

- **Renklendirme:** anahtar kelimeler, tipler, `@GPU` / `@CPU`, `marl` / `parallel`, yerleşikler, stringler, yorumlar.
- **Yazarken hata gösterme:** derleyicinin kendisi dil sunucusu olarak çalışır (`marlin --lsp`); editörün gösterdiği hatalar derleyicininkilerle aynıdır. Import edilen dosyalardaki hatalar da gösterilir.
- **Derle ve Çalıştır:** editörün sağ üstündeki ▷ düğmesi ya da `Ctrl+Alt+R`.
- **Snippet'ler:** `main`, `func`, `struct`, `pfor` (paralel döngü), `marl`, `httpserve`, `pgquery`, `sqlopen` ...
- `//` ile yorum satırı (`Ctrl+/`), parantez eşleme ve otomatik kapanma.

## Ayarlar

| Ayar | Anlamı |
|---|---|
| `marlin.compilerPath` | `marlin`'ın yolu. Boşsa çalışma alanındaki `cmake-build-debug/marlin`, sonra `PATH`. |
| `marlin.runArgs` | Derle ve Çalıştır için ek seçenekler (ör. `["-O2"]`). |
| `marlin.languageServer` | Dil sunucusunu kapatmak için `false`. |

## Geliştirme

```
npm install
npm run compile     # src/ → out/
npm run package     # marlin-<sürüm>.vsix
```

Kaynaktan denemek için bu dizini VS Code'da açıp F5 (Extension Development Host).
