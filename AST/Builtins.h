#ifndef BUILTINS_H
#define BUILTINS_H

#include <string>

// ---------------------------------------------------------------------------
// Yerleşik fonksiyonlar
//
// Dilde tanımı olmayan, derleyicinin doğrudan tanıdığı fonksiyonlar. Sıradan
// fonksiyon olamazlar çünkü birden çok tiple çalışırlar (print(5), print("a");
// sqrt(float), sqrt(float64)) ve dilde aşırı yükleme yoktur.
//
//   * print, time: çalışma zamanı kütüphanesindeki (runtime/minik_rt.c) C
//     fonksiyonlarına çağrı olarak üretilir.
//   * Matematik: MLIR'in math/arith operasyonlarına çevrilir; C çağrısı
//     yazılmaz. Böylece aynı işlem CPU'da ve GPU'da hedefin kendi
//     karşılığına iner.
//   * String işlemleri: çalışma zamanı kütüphanesine çağrı olarak üretilir.
//   * Liste işlemleri: memref operasyonlarına çevrilir (bkz. MLIRListManager).
//   * Matris işlemleri: linalg/tensor operasyonlarına çevrilir
//     (bkz. MLIRMatrixManager).
//
// Kullanıcı aynı isimde bir değişken ya da fonksiyon tanımlarsa onunki
// geçerli olur (yerleşik gölgelenir).
// ---------------------------------------------------------------------------
enum class Builtin {
    None,
    Print,
    Time,                     // time(): saniye cinsinden zaman (float64); süre ölçmek için
    // Tek argümanlı, kayan nokta sonuçlu
    Sqrt, Sin, Cos, Tan, Atan, Tanh, Exp, Log, Log2, Log10,
    Floor, Ceil, Round, Trunc,
    // İki argümanlı, kayan nokta sonuçlu
    Pow, Atan2,
    // Tamsayı ve kayan nokta; sonuç argümanın tipinde
    Abs, Min, Max,
    // Listeler: push(l, x), pop(l).  len liste, dizi ve string alır;
    // free liste ya da stringin belleğini bırakır.
    Push, Pop, Len, Free,
    // Stringler: str(x) değeri metne çevirir (print'in yazdığı biçimde);
    // to_int(s) / to_float(s) metni sayıya çevirir.
    Str, ToInt, ToFloat,
    // Matrisler
    Zeros, Ones, Fill,        // oluşturma: zeros(d1, ..., dn), ones(...), fill(d1, ..., dn, deger)
    Shape,                    // shape(m, i): i. boyutun uzunluğu
    Rows, Cols,               // iki boyutlu matriste shape(m, 0) ve shape(m, 1)
    Matmul, Transpose, Sum,   // matris çarpımı, devrik, elemanların toplamı
};

inline Builtin lookupBuiltin(const std::string& name) {
    static const struct { const char* name; Builtin builtin; } table[] = {
        {"print", Builtin::Print}, {"time", Builtin::Time},
        {"sqrt", Builtin::Sqrt},   {"sin", Builtin::Sin},     {"cos", Builtin::Cos},
        {"tan", Builtin::Tan},     {"atan", Builtin::Atan},   {"tanh", Builtin::Tanh},
        {"exp", Builtin::Exp},     {"log", Builtin::Log},     {"log2", Builtin::Log2},
        {"log10", Builtin::Log10}, {"floor", Builtin::Floor}, {"ceil", Builtin::Ceil},
        {"round", Builtin::Round}, {"trunc", Builtin::Trunc},
        {"pow", Builtin::Pow},     {"atan2", Builtin::Atan2},
        {"abs", Builtin::Abs},     {"min", Builtin::Min},     {"max", Builtin::Max},
        {"push", Builtin::Push},   {"pop", Builtin::Pop},     {"len", Builtin::Len},
        {"free", Builtin::Free},
        {"str", Builtin::Str},     {"to_int", Builtin::ToInt}, {"to_float", Builtin::ToFloat},
        {"zeros", Builtin::Zeros}, {"ones", Builtin::Ones},   {"fill", Builtin::Fill},
        {"rows", Builtin::Rows},   {"cols", Builtin::Cols},   {"shape", Builtin::Shape},
        {"matmul", Builtin::Matmul}, {"transpose", Builtin::Transpose}, {"sum", Builtin::Sum},
    };
    for (const auto& entry : table)
        if (name == entry.name) return entry.builtin;
    return Builtin::None;
}

#endif //BUILTINS_H
