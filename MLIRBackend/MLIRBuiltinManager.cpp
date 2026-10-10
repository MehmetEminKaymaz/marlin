#include "MLIRBuiltinManager.h"

#include "llvm/ADT/SmallVector.h"
#include "mlir/Dialect/Arith/IR/Arith.h"
#include "mlir/Dialect/LLVMIR/LLVMDialect.h"
#include "mlir/Dialect/Math/IR/Math.h"

using namespace mlir;

// ===========================================================================
// Matematik
// ===========================================================================

Value MLIRBuiltinManager::buildMath(Builtin builtin, llvm::ArrayRef<Value> args,
                                    const SemType& argType, Location loc) {
    OpBuilder& b = ctx.builder;
    const bool isFloat = argType.isFloat();
    const bool isSigned = argType.isInt() && argType.isSigned;

    switch (builtin) {
        case Builtin::Sqrt:  return b.create<math::SqrtOp>(loc, args[0]);
        case Builtin::Sin:   return b.create<math::SinOp>(loc, args[0]);
        case Builtin::Cos:   return b.create<math::CosOp>(loc, args[0]);
        case Builtin::Tan:   return b.create<math::TanOp>(loc, args[0]);
        case Builtin::Atan:  return b.create<math::AtanOp>(loc, args[0]);
        case Builtin::Tanh:  return b.create<math::TanhOp>(loc, args[0]);
        case Builtin::Exp:   return b.create<math::ExpOp>(loc, args[0]);
        case Builtin::Log:   return b.create<math::LogOp>(loc, args[0]);
        case Builtin::Log2:  return b.create<math::Log2Op>(loc, args[0]);
        case Builtin::Log10: return b.create<math::Log10Op>(loc, args[0]);
        case Builtin::Floor: return b.create<math::FloorOp>(loc, args[0]);
        case Builtin::Ceil:  return b.create<math::CeilOp>(loc, args[0]);
        case Builtin::Round: return b.create<math::RoundOp>(loc, args[0]);
        case Builtin::Trunc: return b.create<math::TruncOp>(loc, args[0]);

        case Builtin::Pow:   return b.create<math::PowFOp>(loc, args[0], args[1]);
        case Builtin::Atan2: return b.create<math::Atan2Op>(loc, args[0], args[1]);

        case Builtin::Abs:
            if (isFloat) return b.create<math::AbsFOp>(loc, args[0]);
            if (isSigned) return b.create<math::AbsIOp>(loc, args[0]);
            return args[0]; // işaretsiz değer zaten mutlak
        case Builtin::Min:
            if (isFloat) return b.create<arith::MinimumFOp>(loc, args[0], args[1]);
            if (isSigned) return b.create<arith::MinSIOp>(loc, args[0], args[1]);
            return b.create<arith::MinUIOp>(loc, args[0], args[1]);
        case Builtin::Max:
            if (isFloat) return b.create<arith::MaximumFOp>(loc, args[0], args[1]);
            if (isSigned) return b.create<arith::MaxSIOp>(loc, args[0], args[1]);
            return b.create<arith::MaxUIOp>(loc, args[0], args[1]);

        // Matematik olmayan yerleşikler başka yerde üretilir.
        case Builtin::None:
        case Builtin::Print:
        case Builtin::Push: case Builtin::Pop: case Builtin::Len: case Builtin::Free:
        case Builtin::Zeros: case Builtin::Ones: case Builtin::Fill:
        case Builtin::Shape: case Builtin::Rows: case Builtin::Cols:
        case Builtin::Matmul: case Builtin::Transpose: case Builtin::Sum:
            break;
    }
    return nullptr;
}

// ===========================================================================
// print
// ===========================================================================

void MLIRBuiltinManager::printChar(char c, Location loc) {
    Value code = arith.buildIntConstant(static_cast<unsigned char>(c),
                                        ctx.builder.getI32Type(), loc);
    runtime.call("minik_print_char", {code}, loc);
}

// Küçük tamsayılar (bool, char, int8 ...) C tarafına geçmeden önce
// genişletilir: 32 bitten dar argümanların üst bitlerini kimin dolduracağı
// çağrı kuralına bağlıdır; genişleterek bu belirsizlikten kaçınılır.
void MLIRBuiltinManager::printValue(Value value, const SemType& type, Location loc) {
    OpBuilder& b = ctx.builder;

    switch (type.kind) {
        case SemType::Kind::Int:
            if (type.isSigned) {
                if (type.width < 64) value = b.create<arith::ExtSIOp>(loc, b.getI64Type(), value);
                runtime.call("minik_print_i64", {value}, loc);
            } else {
                if (type.width < 64) value = b.create<arith::ExtUIOp>(loc, b.getI64Type(), value);
                runtime.call("minik_print_u64", {value}, loc);
            }
            return;

        case SemType::Kind::Float:
            // float ve float64 ayrı yazdırılır: her biri kendi duyarlığında
            // en kısa gösterimle çıkar (3.14, 3.140000104904175 değil).
            runtime.call(type.width == 32 ? "minik_print_f32" : "minik_print_f64", {value}, loc);
            return;

        case SemType::Kind::Bool:
            value = b.create<arith::ExtUIOp>(loc, b.getI32Type(), value);
            runtime.call("minik_print_bool", {value}, loc);
            return;

        case SemType::Kind::Char:
            value = b.create<arith::ExtUIOp>(loc, b.getI32Type(), value);
            runtime.call("minik_print_char", {value}, loc);
            return;

        case SemType::Kind::String: {
            // {ptr, i64} → (ptr, uzunluk)
            Value ptr = b.create<LLVM::ExtractValueOp>(loc, value, llvm::ArrayRef<int64_t>{0});
            Value len = b.create<LLVM::ExtractValueOp>(loc, value, llvm::ArrayRef<int64_t>{1});
            runtime.call("minik_print_str", {ptr, len}, loc);
            return;
        }

        default:
            return; // Sema diğer tipleri reddeder
    }
}

void MLIRBuiltinManager::buildPrint(llvm::ArrayRef<Value> args, llvm::ArrayRef<SemType> types,
                                    Location loc) {
    // Satır bölünmeden yazılsın: paralel döngüde başka bir iş parçacığının
    // çıktısı araya girmesin.
    runtime.call("minik_print_begin", {}, loc);
    for (size_t i = 0; i < args.size(); ++i) {
        if (i > 0) printChar(' ', loc);
        printValue(args[i], types[i], loc);
    }
    printChar('\n', loc);
    runtime.call("minik_print_end", {}, loc);
}
