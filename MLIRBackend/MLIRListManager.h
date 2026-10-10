#ifndef MLIRLISTMANAGER_H
#define MLIRLISTMANAGER_H

#include "CodegenContext.h"
#include "MLIRRuntime.h"
#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/STLFunctionalExtras.h"

// ---------------------------------------------------------------------------
// MLIRListManager — list<T> (büyüyebilen liste)
//
// GÖSTERİM
//
//   liste değeri   memref<memref<? x T>>    yığın belleğinde tek elemanlı "hücre"
//   hücrenin içi   memref<? x T>            veri; boyutu = listenin UZUNLUĞU
//
//   degisken a ──┐
//                ├──▶ [ hücre ] ──▶ [ e0 e1 e2 ... | boş kapasite ]
//   degisken b ──┘
//
// Başvuru semantiği hücreden gelir: `b = a` yalnızca hücrenin adresini
// kopyalar; push ile veri başka yere taşınsa bile ikisi de aynı hücreden
// okuduğu için güncel veriyi görür.
//
// Veri sıradan bir memref olduğu için eleman erişimi, statik dizilerle aynı
// memref.load / memref.store işlemleridir. Liste üzerindeki bir döngü
// "N elemanlı memref üzerinde scf.for" olarak görünür; ileride GPU'ya
// gönderilecek döngüler için istenen biçim budur.
//
// KAPASİTE
//
// Kapasite ayrıca saklanmaz; uzunluktan türetilir:
//
//     kapasite(n) = n <= 4 ? 4 : n'den küçük olmayan ilk 2'nin kuvveti
//
// Ayrılan bellek her zaman en az bu kadardır. push, uzunluk kapasiteye
// ulaştığında (n >= 4 ve n ikinin kuvveti) belleği iki katına çıkarır; böylece
// n eleman eklemek toplamda n ile orantılı sürer. Uzunluk değiştiğinde veri
// memref'i aynı belleğe bakan yeni boyutlu bir görünümle (reinterpret_cast)
// değiştirilir.
//
// BELLEK
//
// Listeler otomatik olarak geri verilmez; `free(l)` ile elle bırakılır.
// ---------------------------------------------------------------------------
class MLIRListManager {
    CodegenContext& ctx;
    MLIRRuntime& runtime;

public:
    MLIRListManager(CodegenContext& context, MLIRRuntime& runtimeCalls)
        : ctx(context), runtime(runtimeCalls) {}

    /// Verilen elemanlarla (boş olabilir) yeni bir liste oluşturur.
    mlir::Value create(mlir::Type elementType, llvm::ArrayRef<mlir::Value> elements,
                       mlir::Location loc);

    /// Eleman sayısı (index tipinde).
    mlir::Value length(mlir::Value list, mlir::Location loc);

    /// liste[index]. İndeks sınır dışındaysa program hata mesajıyla durur;
    /// `line` o mesajda gösterilen kaynak satırıdır.
    mlir::Value loadElement(mlir::Value list, mlir::Value index, int line, mlir::Location loc);
    void storeElement(mlir::Value list, mlir::Value index, mlir::Value value, int line,
                      mlir::Location loc);

    void push(mlir::Value list, mlir::Value value, mlir::Location loc);
    /// Son elemanı çıkarıp döndürür; liste boşsa program hata mesajıyla durur.
    mlir::Value pop(mlir::Value list, int line, mlir::Location loc);
    void free(mlir::Value list, mlir::Location loc);

private:
    mlir::Value loadData(mlir::Value list, mlir::Location loc);
    /// `data` ile aynı belleğe bakan, `newLength` elemanlı görünüm.
    mlir::Value resized(mlir::Value data, mlir::Value newLength, mlir::Location loc);
    void checkIndex(mlir::Value index, mlir::Value length, int line, mlir::Location loc);
    /// scf.if %cond { body }
    void emitIf(mlir::Value cond, mlir::Location loc, llvm::function_ref<void()> body);
    mlir::Value toI64(mlir::Value index, mlir::Location loc);
};

#endif //MLIRLISTMANAGER_H
