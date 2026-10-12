//
// Created by Eminf on 19.07.2026.
//

#ifndef MLIRTYPEMANAGER_H
#define MLIRTYPEMANAGER_H

#include "CodegenContext.h"
#include "../AST/SemType.h"
#include "llvm/ADT/SmallVector.h"
#include "mlir/IR/BuiltinTypes.h"
#include "mlir/IR/Types.h"
#include "mlir/Dialect/LLVMIR/LLVMDialect.h"
#include "mlir/Dialect/LLVMIR/LLVMTypes.h"

// Semantik tipleri (SemType) MLIR tiplerine çevirir.
//
//   int8..int64, uint8..uint64 → iN   (MLIR tamsayıları işaretsizdir; işaret
//                                      bilgisi SemType'ta kalır ve işlem
//                                      seçilirken kullanılır)
//   float / float64            → f32 / f64
//   bool / char                → i1 / i8
//   string                     → i64   (çalışma zamanındaki string bloğunun adresi)
//   T[N]                       → memref<N x T>
//   list<T>                    → memref<memref<? x T>>   (bkz. MLIRListManager)
//   matrix<T, N>               → memref<? x ... x T> (N boyut); ifade içinde tensor
//                                (bkz. MLIRMatrixManager)
//   (A, B) -> R                → (A, B) -> R
//   struct                     → karşılığı yok; yapraklarına ayrılır (bkz. convertFunction)
//   void                       → boş Type (çağıran taraf ele alır)
class MLIRTypeManager {
    CodegenContext& ctx;
public:
    MLIRTypeManager(CodegenContext& context) : ctx(context) {}

    mlir::Type convert(const SemType& type) {
        mlir::OpBuilder& builder = ctx.builder;

        switch (type.kind) {
            case SemType::Kind::Int:   return builder.getIntegerType(type.width);
            case SemType::Kind::Float: return type.width == 32 ? mlir::Type(builder.getF32Type())
                                                               : mlir::Type(builder.getF64Type());
            case SemType::Kind::Bool:  return builder.getI1Type();
            case SemType::Kind::Char:  return builder.getIntegerType(8);
            // String, çalışma zamanındaki string bloğunun adresidir (bkz.
            // runtime/minik_rt.c). Adres tamsayı olarak taşınır: böylece
            // sıradan bir skaler gibi yuvada, dizide ve listede saklanabilir
            // (LLVM işaretçisi memref elemanı olamaz).
            case SemType::Kind::String:
                return builder.getI64Type();
            case SemType::Kind::Array:
                return mlir::MemRefType::get({type.arraySize}, convert(type.elementType()));
            case SemType::Kind::List:
                return listCellType(convert(type.elementType()));
            case SemType::Kind::Matrix:
                return matrixBufferType(convert(type.elementType()), type.rank);
            case SemType::Kind::Function:
                return convertFunction(type);
            case SemType::Kind::Struct: // tek bir değer değildir; yaprakları çevrilir
            case SemType::Kind::Void:
            case SemType::Kind::Error:
                return nullptr;
        }
        return nullptr;
    }

    // Listenin verisi: uzunluğu çalışırken bilinen tek boyutlu memref.
    static mlir::MemRefType listDataType(mlir::Type elementType) {
        return mlir::MemRefType::get({mlir::ShapedType::kDynamic}, elementType);
    }

    // Liste değeri: veri memref'ini tutan, yığın belleğindeki tek elemanlı
    // hücre. Listeyi gösteren herkes aynı hücreyi paylaşır.
    static mlir::MemRefType listCellType(mlir::Type elementType) {
        return mlir::MemRefType::get({}, listDataType(elementType));
    }

    // Matrisin bellekteki hali: değişkenlerde, parametrelerde ve dönüş
    // değerlerinde bu tip kullanılır. Boyut sayısı (rank) tipte sabittir;
    // her boyutun uzunluğu çalışırken belli olur (?).
    static mlir::MemRefType matrixBufferType(mlir::Type elementType, int rank) {
        llvm::SmallVector<int64_t, 4> shape(rank, mlir::ShapedType::kDynamic);
        return mlir::MemRefType::get(shape, elementType);
    }

    // Matrisin ifade içindeki hali: işlemler tensor üzerinde yapılır.
    static mlir::RankedTensorType matrixTensorType(mlir::Type elementType, int rank) {
        llvm::SmallVector<int64_t, 4> shape(rank, mlir::ShapedType::kDynamic);
        return mlir::RankedTensorType::get(shape, elementType);
    }

    // Struct'ın MLIR'de karşılığı yoktur: yapraklarına ayrılarak taşınır
    // (bkz. SemType.h, leavesOf). Fonksiyon imzasında:
    //
    //   * struct parametre  → yaprak başına bir parametre
    //   * struct dönüş      → yaprak başına bir sonuç. Dizi yaprakları sonuç
    //                         olarak dönemez (dizi yığıtta yaşar); onların
    //                         yerini çağıran ayırır ve imzanın sonuna eklenen
    //                         parametrelerle verir, fonksiyon oraya yazar.
    //
    //   func f(Kayit k) -> Kayit        Kayit { int no; int[3] d; }
    //     →  (i32, memref<3xi32>, memref<3xi32>) -> i32
    //         k.no  k.d            dönen .d         dönen .no
    mlir::FunctionType convertFunction(const SemType& type) {
        llvm::SmallVector<mlir::Type, 4> inputs;
        for (size_t i = 0; i < type.paramCount(); ++i)
            for (const SemLeaf& leaf : leavesOf(type.paramType(i)))
                inputs.push_back(convert(leaf.type));

        llvm::SmallVector<mlir::Type, 1> results;
        for (const SemLeaf& leaf : leavesOf(type.returnType())) {
            if (type.returnType().isStruct() && leaf.type.isArray())
                inputs.push_back(convert(leaf.type));
            else
                results.push_back(convert(leaf.type));
        }

        return mlir::FunctionType::get(&ctx.context, inputs, results);
    }
};



#endif //MLIRTYPEMANAGER_H
