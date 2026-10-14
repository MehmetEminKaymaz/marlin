// MLIRGenerator — stringlerin belleği (başvuru sayımı)
//
// String, çalışma zamanındaki bir bloğun adresidir ve değişmez; bu yüzden
// kopyalanmaz, PAYLAŞILIR. Bloğun başındaki sayaç onu kaç yerin tuttuğunu
// söyler (bkz. runtime/minik_rt.c); sıfıra inince bellek geri verilir. Sayacı
// burada üretilen kod yönetir; kullanıcı string için `free` yazmaz.
//
// KURALLAR
//
//   * Yeni üretilen her string (a + b, str(x), fonksiyonun döndürdüğü değer,
//     pop) sayacı 1 olarak doğar; o başvuru içinde bulunulan deyimindir
//     (GEÇİCİ). Bir yere yazılmazsa deyim bitince bırakılır (flushTemporaries).
//     `a + b + c` içindeki `a + b` böyle bırakılır.
//
//   * String bir yere YAZILIRKEN (değişken, dizi ya da liste elemanı, dönüş
//     değeri) sahiplenilir (ownString): geçiciyse başvurusu devralınır,
//     başkasınınsa (bir değişkenin değeri) sayacı artırılır.
//
//   * Üzerine yazılan eski değer ve kapsamı biten değişken bırakılır.
//
//   * Değişkenden okunan değer ödünçtür: sayaç değişmez. Dizi ve liste
//     elemanından okunan değer ise sayacı artırılarak geçici yapılır; eleman
//     aynı deyimde listeden çıkarılsa bile değer geçerli kalır.
//
//   * Parametre, fonksiyonun yerel değişkenidir: girişte sayacı artırılır,
//     çıkışta bırakılır (böylece fonksiyon içinde yeniden atanabilir).
//
//   * `return` değeri sahiplenir; o başvuru çağırana geçer.
//
// Literaller ve boş string sayılmaz (çalışma zamanı onları tanır).
//
// STRING DİZİLERİ
//
// string[N] bir değerdir (atama kopyalar); her elemanı bir başvuru tutar.
// Kopyalanırken elemanların sayaçları artar (copyArray), dizi yok olurken
// hepsi bırakılır (releaseStringArray).

#include "MLIRGenerator.h"

#include "mlir/Dialect/Arith/IR/Arith.h"
#include "mlir/Dialect/LLVMIR/LLVMDialect.h"
#include "mlir/Dialect/MemRef/IR/MemRef.h"
#include "mlir/Dialect/SCF/IR/SCF.h"
#include <algorithm>

void MLIRGenerator::retainString(mlir::Value value, mlir::Location loc) {
    runtime.call("minik_str_retain", {value}, loc);
}

void MLIRGenerator::releaseString(mlir::Value value, mlir::Location loc) {
    runtime.call("minik_str_release", {value}, loc);
}

mlir::Value MLIRGenerator::stringResult(mlir::Value value) {
    temporaries.push_back({value, SemType::String()});
    return value;
}

mlir::Value MLIRGenerator::ownString(mlir::Value value, mlir::Location loc) {
    auto temporary = std::find_if(temporaries.begin(), temporaries.end(), [&](const Owned& owned) {
        return owned.value == value && owned.type.isString();
    });
    if (temporary != temporaries.end()) {
        temporaries.erase(temporary);
        return value;
    }
    // Literal (adresi alınmış global sabit): sayılmaz.
    if (value.getDefiningOp<mlir::LLVM::PtrToIntOp>()) return value;
    retainString(value, loc);
    return value;
}

void MLIRGenerator::forEachIndex(int64_t count, mlir::Location loc,
                                 llvm::function_ref<void(mlir::Value index)> body) {
    mlir::Value begin = ctx.builder.create<mlir::arith::ConstantIndexOp>(loc, 0);
    mlir::Value end = ctx.builder.create<mlir::arith::ConstantIndexOp>(loc, count);
    mlir::Value step = ctx.builder.create<mlir::arith::ConstantIndexOp>(loc, 1);
    ctx.builder.create<mlir::scf::ForOp>(
        loc, begin, end, step, mlir::ValueRange(),
        [&](mlir::OpBuilder& b, mlir::Location l, mlir::Value index, mlir::ValueRange) {
            body(index);
            b.create<mlir::scf::YieldOp>(l);
        });
}

void MLIRGenerator::copyArray(mlir::Value source, mlir::Value target, const SemType& type,
                              bool releaseOld, mlir::Location loc) {
    if (!type.elementType().isString()) {
        ctx.builder.create<mlir::memref::CopyOp>(loc, source, target);
        return;
    }
    // Önce yeni değer tutulur, sonra eskisi bırakılır: kaynak ile hedef aynı
    // dizi olsa da (a = a) doğru çalışır.
    forEachIndex(type.arraySize, loc, [&](mlir::Value index) {
        mlir::Value value = ctx.builder.create<mlir::memref::LoadOp>(
            loc, source, mlir::ValueRange(index));
        retainString(value, loc);
        mlir::Value old;
        if (releaseOld)
            old = ctx.builder.create<mlir::memref::LoadOp>(loc, target, mlir::ValueRange(index));
        ctx.builder.create<mlir::memref::StoreOp>(loc, value, target, mlir::ValueRange(index));
        if (releaseOld) releaseString(old, loc);
    });
}

void MLIRGenerator::releaseStringArray(mlir::Value array, const SemType& type, mlir::Location loc) {
    forEachIndex(type.arraySize, loc, [&](mlir::Value index) {
        releaseString(ctx.builder.create<mlir::memref::LoadOp>(loc, array, mlir::ValueRange(index)),
                      loc);
    });
}

// `isSlot`: değer bir değişkenin yuvasında (kapsam sonu); değilse değerin
// kendisi (deyimin geçicisi).
void MLIRGenerator::releaseOwned(const Owned& owned, bool isSlot, mlir::Location loc) {
    if (owned.type.isArray()) {
        releaseStringArray(owned.value, owned.type, loc);
        return;
    }
    mlir::Value value = isSlot
        ? ctx.builder.create<mlir::memref::LoadOp>(loc, owned.value).getResult()
        : owned.value;
    if (owned.type.isMatrix()) matrixManager.freeBuffer(value, loc);
    else releaseString(value, loc);
}
