#ifndef MLIRMATRIXMANAGER_H
#define MLIRMATRIXMANAGER_H

#include "CodegenContext.h"
#include "MLIRRuntime.h"
#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/STLFunctionalExtras.h"
#include "llvm/ADT/SmallVector.h"

// ---------------------------------------------------------------------------
// MLIRMatrixManager — matrix<T, N> işlemlerinin MLIR karşılıkları
//
// Matrisin boyut SAYISI (rank) tipinde sabittir: matrix<T, 1> bir vektör,
// matrix<T> = matrix<T, 2> bir matris, matrix<T, 3> üç boyutlu bir tablodur.
// Bu sınıftaki işlemler boyut sayısını değerin MLIR tipinden okur; matmul ve
// transpose dışında hepsi her boyut sayısı için çalışır.
//
// İKİ GÖSTERİM
//
//   bellekte    memref<? x ... x T>   değişkenler, parametreler, dönüş değerleri
//   ifadede     tensor<? x ... x T>   işlemlerin girdi ve çıktıları
//
// Bir ifade hesaplanırken matrisler tensor olarak görülür ve işlemler
// (linalg.generic, linalg.matmul ...) tensor üzerinde üretilir. Tensor'un
// adresi yoktur; bu yüzden "iki matris aynı belleği paylaşıyor mu" sorusu hiç
// doğmaz ve işlemler üzerinde dönüşüm yapmak güvenlidir.
//
// Köprüler:
//   toTensor  (bufferization.to_tensor)   bellek → tensor
//   toBuffer  (bufferization.to_memref)   tensor → bellek
//
// İndirme hattındaki one-shot-bufferize pass'i tensor'ları yeniden belleğe
// yerleştirir; ardından linalg işlemleri döngülere açılır.
//
// SONUÇ BELLEĞİ
//
// Her işlemin sonucu YENİ ayrılmış bir belleğe yazılır (allocate). İşlem, o
// belleği "yazılabilir" bir tensor olarak çıktı (outs) alır; bufferization
// sonucu doğrudan oraya yazar. Böylece belleğin sahibi her zaman bellidir:
// Result::buffer'ı kim aldıysa bırakmaktan o sorumludur (bkz. MLIRGenerator,
// matris sahipliği).
//
// FÜZYON
//
// Eleman eleman işlemler tek tek üretilmez: çağıran taraf bir ifadedeki
// bütün eleman eleman işlemleri tek bir `elementwise` çağrısında, gövdesi
// tüm hesabı içeren tek bir linalg.generic olarak üretir. `a + b * c` bu
// yüzden tek döngü yuvasıdır ve geçici matris ayırmaz.
// ---------------------------------------------------------------------------
class MLIRMatrixManager {
    CodegenContext& ctx;
    MLIRRuntime& runtime;

public:
    MLIRMatrixManager(CodegenContext& context, MLIRRuntime& runtimeCalls)
        : ctx(context), runtime(runtimeCalls) {}

    /// Yeni hesaplanmış bir matris: ifadede kullanılacak tensor ve arkasındaki
    /// (yeni ayrılmış, henüz sahipsiz) bellek.
    struct Result {
        mlir::Value tensor;
        mlir::Value buffer;
    };

    /// Eleman değerlerinden (her girdi matrisinden bir tane) sonucu hesaplar.
    using ElementFn = llvm::function_ref<mlir::Value(mlir::ValueRange elements)>;

    // --- köprüler ve boyut ---
    mlir::Value toTensor(mlir::Value buffer, bool writable, mlir::Location loc);
    mlir::Value toBuffer(mlir::Value tensor, mlir::Location loc);
    /// tensor'un `dimension` numaralı boyutunun uzunluğu, index tipinde.
    mlir::Value dim(mlir::Value tensor, unsigned dimension, mlir::Location loc);

    // --- bellek ---
    /// Yeni matris belleği; `sizes`: her boyutun uzunluğu (index tipinde).
    /// @GPU fonksiyonda GPU belleğinden, aksi halde yığın belleğinden ayrılır.
    mlir::Value allocateBuffer(llvm::ArrayRef<mlir::Value> sizes, mlir::Type elementType,
                               mlir::Location loc);
    /// allocateBuffer ile (aynı tür fonksiyonda) ayrılmış belleği bırakır.
    void freeBuffer(mlir::Value buffer, mlir::Location loc);
    /// Belleğin nerede olacağı açıkça verilir: GPU'da (onDevice) ya da CPU'da.
    mlir::Value allocateIn(llvm::ArrayRef<mlir::Value> sizes, mlir::Type elementType,
                           bool onDevice, mlir::Location loc);

    // --- CPU <-> GPU (yalnızca GPU hedefi açıkken kullanılır) ---
    /// `buffer` ile aynı şekilde, CPU (onDevice = false) ya da GPU belleğinde
    /// yeni bir bellek ayırır.
    mlir::Value allocateLike(mlir::Value buffer, bool onDevice, mlir::Location loc);
    void free(mlir::Value buffer, bool onDevice, mlir::Location loc);
    /// CPU belleğindeki matrisin GPU'daki kopyası.
    mlir::Value upload(mlir::Value hostBuffer, mlir::Location loc);
    /// GPU belleğindeki matrisin CPU'daki kopyası.
    mlir::Value download(mlir::Value deviceBuffer, mlir::Location loc);

    // --- oluşturma ---
    /// Bütün elemanları `value` olan matris.
    Result filled(llvm::ArrayRef<mlir::Value> sizes, mlir::Value value, mlir::Location loc);

    // --- işlemler (her boyut sayısı için) ---
    /// Aynı şekildeki girdilerden eleman eleman yeni bir matris üretir.
    /// Şekiller uyuşmazsa program hata mesajıyla durur (`line`: kaynak satırı).
    Result elementwise(llvm::ArrayRef<mlir::Value> inputs, mlir::Type resultElementType,
                       int line, ElementFn body, mlir::Location loc);
    /// Eleman eleman hesaplanan değerlerin toplamı (skaler). Ara matris ayrılmaz.
    mlir::Value sum(llvm::ArrayRef<mlir::Value> inputs, mlir::Type elementType,
                    int line, ElementFn body, mlir::Location loc);

    // --- yalnızca iki boyutlu matrisler ---
    Result matmul(mlir::Value lhs, mlir::Value rhs, int line, mlir::Location loc);
    Result transpose(mlir::Value input, mlir::Location loc);

    // --- eleman erişimi (bellek üzerinde, sınır denetimli) ---
    /// `indices`: her boyut için bir indeks (index tipinde).
    mlir::Value loadElement(mlir::Value buffer, llvm::ArrayRef<mlir::Value> indices, int line,
                            mlir::Location loc);
    void storeElement(mlir::Value buffer, llvm::ArrayRef<mlir::Value> indices, mlir::Value value,
                      int line, mlir::Location loc);

private:
    Result allocate(llvm::ArrayRef<mlir::Value> sizes, mlir::Type elementType,
                    mlir::Location loc);
    mlir::Value sumOnDevice(llvm::ArrayRef<mlir::Value> inputs, mlir::Type elementType,
                            ElementFn body, mlir::Location loc);
    void checkSameShape(llvm::ArrayRef<mlir::Value> inputs, int line, mlir::Location loc);
    void checkIndex(mlir::Value buffer, llvm::ArrayRef<mlir::Value> indices, int line,
                    mlir::Location loc);
    /// Hata mesajı için değerleri çalışma zamanına bildirir (bkz. marlin_rt.c:
    /// marlin_report_push). `slot`: 0 ya da 1 numaralı liste.
    void report(int slot, llvm::ArrayRef<mlir::Value> indexValues, mlir::Location loc);
    llvm::SmallVector<mlir::Value, 4> shapeOf(mlir::Value tensor, mlir::Location loc);
    mlir::Value zeroOf(mlir::Type elementType, mlir::Location loc);
};

#endif //MLIRMATRIXMANAGER_H
