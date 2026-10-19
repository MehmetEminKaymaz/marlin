#include "GpuSpirv.h"

#include "../runtime/gpu_package.h"

#include <cstdlib>
#include <functional>

#include "mlir/Conversion/ArithToSPIRV/ArithToSPIRV.h"
#include "mlir/Conversion/ControlFlowToSCF/ControlFlowToSCF.h"
#include "mlir/Conversion/UBToSPIRV/UBToSPIRV.h"
#include "mlir/Dialect/UB/IR/UBOps.h"
#include "mlir/IR/Dominance.h"
#include "mlir/Transforms/CFGToSCF.h"
#include "mlir/Conversion/FuncToSPIRV/FuncToSPIRV.h"
#include "mlir/Conversion/GPUToSPIRV/GPUToSPIRV.h"
#include "mlir/Conversion/MathToSPIRV/MathToSPIRV.h"
#include "mlir/Conversion/MemRefToSPIRV/MemRefToSPIRV.h"
#include "mlir/Conversion/SCFToSPIRV/SCFToSPIRV.h"
#include "mlir/Conversion/VectorToSPIRV/VectorToSPIRV.h"
#include "mlir/Dialect/Arith/IR/Arith.h"
#include "mlir/Dialect/Arith/Transforms/Passes.h"
#include "mlir/Dialect/Func/IR/FuncOps.h"
#include "mlir/Dialect/GPU/IR/GPUDialect.h"
#include "mlir/Dialect/LLVMIR/NVVMDialect.h"
#include "mlir/Dialect/MemRef/IR/MemRef.h"
#include "mlir/Dialect/SCF/IR/SCF.h"
#include "mlir/Dialect/SPIRV/IR/SPIRVAttributes.h"
#include "mlir/Dialect/SPIRV/IR/SPIRVDialect.h"
#include "mlir/Dialect/SPIRV/IR/SPIRVOps.h"
#include "mlir/Dialect/SPIRV/IR/TargetAndABI.h"
#include "mlir/Dialect/SPIRV/Transforms/Passes.h"
#include "mlir/Dialect/SPIRV/Transforms/SPIRVConversion.h"
#include "mlir/Dialect/Vector/IR/VectorOps.h"
#include "mlir/IR/Diagnostics.h"
#include "mlir/Pass/PassManager.h"
#include "mlir/Target/SPIRV/Serialization.h"
#include "mlir/Transforms/DialectConversion.h"
#include "mlir/Transforms/Passes.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/Support/raw_ostream.h"

namespace marlin {

namespace {

using namespace mlir;

// Sayı parametresinin CPU'daki bayt boyutu (index: 64 bit); desteklenmiyorsa 0.
unsigned scalarBytes(Type type) {
    if (type.isIndex()) return 8;
    if (auto integer = mlir::dyn_cast<IntegerType>(type)) {
        const unsigned width = integer.getWidth();
        if (width == 1) return 1;
        return (width % 8 == 0 && width <= 64) ? width / 8 : 0;
    }
    if (type.isF32()) return 4;
    if (type.isF64()) return 8;
    return 0;
}

// Tampondaki elemanlar: SPIR-V'nin 8/16 bitlik depolaması ayrı aygıt
// özellikleri ister; şimdilik yalnızca 32/64 bitlik sayılar.
bool storableElement(Type type) {
    return type.isF32() || type.isF64() || type.isInteger(32) || type.isInteger(64);
}

// Bir sayı yuvasının (i64) çekirdekteki asıl tipe çevrilmesi. CPU tarafı
// değerin baytlarını yuvanın başına yazar (küçük sonlu), kalanı sıfırdır.
Value fromSlot(OpBuilder& b, Location loc, Value raw, Type type) {
    if (type.isIndex()) return b.create<arith::IndexCastOp>(loc, type, raw);
    if (type.isInteger(64)) return raw;
    if (auto integer = mlir::dyn_cast<IntegerType>(type)) return b.create<arith::TruncIOp>(loc, integer, raw);
    if (type.isF64()) return b.create<arith::BitcastOp>(loc, type, raw);
    Value bits = b.create<arith::TruncIOp>(loc, b.getI32Type(), raw); // f32
    return b.create<arith::BitcastOp>(loc, type, bits);
}

// Düzleştirilmiş memref: tek boyutlu tampon + yuvalardan okunan yerleşim.
struct FlatMemref {
    Value buffer;
    Value offset;
    SmallVector<Value, 2> sizes;
    SmallVector<Value, 2> strides;
};

Value linearIndex(OpBuilder& b, Location loc, const FlatMemref& flat, ValueRange indices) {
    Value index = flat.offset;
    for (auto [i, stride] : llvm::zip(indices, flat.strides)) {
        Value term = b.create<arith::MulIOp>(loc, i, stride);
        index = b.create<arith::AddIOp>(loc, index, term);
    }
    return index;
}

// Eski memref parametresinin bütün kullanımlarını düz tampona çevirir.
LogicalResult rewriteMemrefUses(Value old, const FlatMemref& flat, std::string& error) {
    for (OpOperand& use : llvm::make_early_inc_range(old.getUses())) {
        Operation* user = use.getOwner();
        OpBuilder b(user);
        const Location loc = user->getLoc();
        if (auto load = mlir::dyn_cast<memref::LoadOp>(user)) {
            Value index = linearIndex(b, loc, flat, load.getIndices());
            Value value = b.create<memref::LoadOp>(loc, flat.buffer, ValueRange{index});
            load.getResult().replaceAllUsesWith(value);
            load.erase();
        } else if (auto store = mlir::dyn_cast<memref::StoreOp>(user); store && store.getMemRef() == old) {
            Value index = linearIndex(b, loc, flat, store.getIndices());
            b.create<memref::StoreOp>(loc, store.getValue(), flat.buffer, ValueRange{index});
            store.erase();
        } else if (auto dim = mlir::dyn_cast<memref::DimOp>(user)) {
            std::optional<int64_t> axis = dim.getConstantIndex();
            if (!axis || *axis < 0 || *axis >= static_cast<int64_t>(flat.sizes.size())) {
                error = "memref.dim: boyut sabit degil";
                return failure();
            }
            dim.getResult().replaceAllUsesWith(flat.sizes[*axis]);
            dim.erase();
        } else if (auto load = mlir::dyn_cast<vector::LoadOp>(user)) {
            Value index = linearIndex(b, loc, flat, load.getIndices());
            Value value = b.create<vector::LoadOp>(loc, load.getVectorType(), flat.buffer, ValueRange{index});
            load.getResult().replaceAllUsesWith(value);
            load.erase();
        } else if (auto store = mlir::dyn_cast<vector::StoreOp>(user); store && store.getBase() == old) {
            Value index = linearIndex(b, loc, flat, store.getIndices());
            b.create<vector::StoreOp>(loc, store.getValueToStore(), flat.buffer, ValueRange{index});
            store.erase();
        } else {
            error = "memref parametresinin desteklenmeyen kullanimi: " + user->getName().getStringRef().str();
            return failure();
        }
    }
    return success();
}

// Çekirdeği Vulkan'ın parametre modeline uydurur (bkz. GpuSpirv.h) ve
// tablodaki satırını yazar.
LogicalResult prepareKernel(gpu::GPUFuncOp kernel, GpuKernelPackage& package, std::string& error) {
    MLIRContext* context = kernel.getContext();
    const Location loc = kernel.getLoc();
    auto storage = [&](spirv::StorageClass c) { return spirv::StorageClassAttr::get(context, c); };

    auto blockSize = kernel->getAttrOfType<DenseI32ArrayAttr>("known_block_size");
    if (!blockSize || blockSize.size() != 3) {
        error = "cekirdegin blok boyutu bilinmiyor";
        return failure();
    }
    const llvm::ArrayRef<int32_t> block = blockSize.asArrayRef();
    const int64_t threads = int64_t(block[0]) * block[1] * block[2];
    if (threads > 256) {
        error = "blokta 256'dan fazla is parcacigi";
        return failure();
    }
    if (threads > 128) package.requirements |= MARLIN_GPU_NEEDS_WIDE_GROUP;

    // Yeni parametreler: sayı yuvaları + her memref için bir tampon.
    const FunctionType type = kernel.getFunctionType();
    const Attribute storageBuffer = storage(spirv::StorageClass::StorageBuffer);
    auto bufferOf = [&](Type element) {
        return MemRefType::get({ShapedType::kDynamic}, element, MemRefLayoutAttrInterface{}, storageBuffer);
    };
    SmallVector<Type, 8> inputs{bufferOf(IntegerType::get(context, 64))};
    std::string line = kernel.getName().str() + " " + std::to_string(type.getNumInputs());
    for (Type input : type.getInputs()) {
        if (auto memref = mlir::dyn_cast<MemRefType>(input)) {
            const Type element = memref.getElementType();
            if (!storableElement(element)) {
                error = "tampon eleman tipi desteklenmiyor";
                return failure();
            }
            inputs.push_back(bufferOf(element));
            line += " m" + std::to_string(memref.getRank()) + "x" + std::to_string(scalarBytes(element));
        } else {
            const unsigned bytes = scalarBytes(input);
            if (bytes == 0) {
                error = "parametre tipi desteklenmiyor";
                return failure();
            }
            line += " s" + std::to_string(bytes);
        }
    }

    OpBuilder builder(kernel);
    auto replacement = builder.create<gpu::GPUFuncOp>(loc, kernel.getName(), builder.getFunctionType(inputs, {}));
    for (llvm::StringRef name : {"known_block_size", "known_grid_size"})
        if (Attribute value = kernel->getAttr(name)) replacement->setAttr(name, value);
    replacement->setAttr(gpu::GPUDialect::getKernelFuncAttrName(), builder.getUnitAttr());
    replacement->setAttr(spirv::getEntryPointABIAttrName(), spirv::getEntryPointABIAttr(context, block));
    for (unsigned i = 0; i < inputs.size(); ++i)
        replacement.setArgAttr(i, spirv::getInterfaceVarABIAttrName(),
                               spirv::getInterfaceVarABIAttr(0, i, std::nullopt, context));

    // Girişte: yuvalar okunur, eski parametrelerin yerine geçer.
    Block& entry = kernel.getBody().front();
    Block& newEntry = replacement.getBody().front();
    OpBuilder b = OpBuilder::atBlockBegin(&newEntry);
    Value slots = newEntry.getArgument(0);
    unsigned nextSlot = 0, nextBuffer = 1;
    auto readSlot = [&]() -> Value {
        Value index = b.create<arith::ConstantIndexOp>(loc, nextSlot++);
        return b.create<memref::LoadOp>(loc, slots, ValueRange{index});
    };
    auto readIndex = [&]() -> Value { return b.create<arith::IndexCastOp>(loc, b.getIndexType(), readSlot()); };

    SmallVector<std::pair<Value, FlatMemref>, 4> memrefs;
    for (unsigned i = 0; i < type.getNumInputs(); ++i) {
        Value old = entry.getArgument(i);
        if (auto memref = mlir::dyn_cast<MemRefType>(old.getType())) {
            FlatMemref flat;
            flat.buffer = newEntry.getArgument(nextBuffer++);
            flat.offset = readIndex();
            for (int64_t d = 0; d < memref.getRank(); ++d) flat.sizes.push_back(readIndex());
            for (int64_t d = 0; d < memref.getRank(); ++d) flat.strides.push_back(readIndex());
            memrefs.push_back({old, flat});
        } else {
            old.replaceAllUsesWith(fromSlot(b, loc, readSlot(), old.getType()));
        }
    }

    // Paylaşımlı bellek → Workgroup sınıfında genel değişken (memref.alloc);
    // iş parçacığının özel belleği → Function sınıfında yerel değişken.
    auto reallocate = [&](BlockArgument argument, spirv::StorageClass c) {
        auto memref = mlir::cast<MemRefType>(argument.getType());
        auto placed = MemRefType::get(memref.getShape(), memref.getElementType(), memref.getLayout(), storage(c));
        Value value = c == spirv::StorageClass::Workgroup ? Value(b.create<memref::AllocOp>(loc, placed))
                                                          : Value(b.create<memref::AllocaOp>(loc, placed));
        argument.replaceAllUsesWith(value);
    };
    for (BlockArgument argument : kernel.getWorkgroupAttributions())
        reallocate(argument, spirv::StorageClass::Workgroup);
    for (BlockArgument argument : kernel.getPrivateAttributions())
        reallocate(argument, spirv::StorageClass::Function);

    // Gövde yeni fonksiyona taşınır.
    newEntry.getOperations().splice(newEntry.end(), entry.getOperations());
    Region& from = kernel.getBody();
    Region& to = replacement.getBody();
    to.getBlocks().splice(to.end(), from.getBlocks(), std::next(from.begin()), from.end());

    for (auto& [old, flat] : memrefs)
        if (failed(rewriteMemrefUses(old, flat, error))) return failure();

    kernel.erase();
    package.table += line + "\n";
    return success();
}

// Yapısız akış (cf.br / cf.cond_br: döngüden break, erken çıkış) → scf
// döngüleri ve koşulları. Vulkan gölgelendiricileri yapısal akış ister.
// (MLIR'in lift-cf-to-scf pass'i yalnızca func.func'lara bakar; aynı dönüşüm
// burada gpu.func'lar için de çağrılır.)
LogicalResult liftControlFlow(gpu::GPUModuleOp module) {
    ControlFlowToSCFTransformation transformation;
    DominanceInfo dominance;
    WalkResult result = module.walk<WalkOrder::PostOrder>([&](Operation* op) {
        if (op == module.getOperation()) return WalkResult::advance();
        for (Region& region : op->getRegions())
            if (failed(transformCFGToSCF(region, transformation, dominance))) return WalkResult::interrupt();
        return WalkResult::advance();
    });
    return failure(result.wasInterrupted());
}

// MLIR 19'un scf.for → SPIR-V dönüşümündeki açık: döngünün sonuçları ilk
// değeri verilmemiş bir değişkende (Function sınıfı) tutulur ve ona yalnızca
// döngü gövdesinin sonunda yazılır. Döngü hiç dönmezse (alt sınır ≥ üst
// sınır) okunan değer tanımsızdır: lavapipe 0 verir, NVIDIA sürücüsü bir
// önceki değeri (boş matrisin sum'ı 1 çıkıyordu). Sonuç taşıyan her döngü bir
// koşulun içine alınır; scf.if'in iki dalı da değişkene yazar:
//   scf.if (alt < üst) { scf.for ... } else { ilk değerler }
void guardEmptyLoops(gpu::GPUModuleOp module) {
    llvm::SmallVector<scf::ForOp, 8> loops;
    module.walk([&](scf::ForOp loop) {
        if (loop.getNumResults() > 0) loops.push_back(loop);
    });
    for (scf::ForOp loop : loops) {
        OpBuilder b(loop);
        const Location loc = loop.getLoc();
        // (scf.for sınırları işaretli karşılaştırır.)
        Value runs = b.create<arith::CmpIOp>(loc, arith::CmpIPredicate::slt, loop.getLowerBound(),
                                             loop.getUpperBound());
        auto guard = b.create<scf::IfOp>(loc, loop.getResultTypes(), runs, /*withElseRegion=*/true);
        loop->replaceAllUsesWith(guard.getResults());
        Block* thenBlock = guard.thenBlock();
        loop->moveBefore(thenBlock, thenBlock->end());
        b.setInsertionPointToEnd(thenBlock);
        b.create<scf::YieldOp>(loc, loop.getResults());
        b.setInsertionPointToEnd(guard.elseBlock());
        b.create<scf::YieldOp>(loc, loop.getInitArgs());
    }
}

// Çekirdeklerin derleneceği Vulkan ortamı. Burada izin verilenler üst
// sınırdır; spirv-update-vce modülün gerçekten kullandıklarını yazar,
// çalışma zamanı onları aygıtın özellikleriyle karşılaştırır.
spirv::TargetEnvAttr vulkanTarget(MLIRContext* context) {
    auto triple = spirv::VerCapExtAttr::get(
        spirv::Version::V_1_3, {spirv::Capability::Shader, spirv::Capability::Int64, spirv::Capability::Float64},
        {spirv::Extension::SPV_KHR_storage_buffer_storage_class}, context);
    return spirv::TargetEnvAttr::get(triple, spirv::getDefaultResourceLimits(context), spirv::ClientAPI::Vulkan);
}

// gpu.module (kopyası) → spirv.module. MLIR'in convert-gpu-to-spirv pass'iyle
// aynı desenler, artı matematik (GLSL komutları). Yapısız akış (cf) bilerek
// yoktur: Vulkan gölgelendiricileri yapısal akış ister; kalırsa çeviri
// başarısız olur ve çekirdek CPU'ya düşer.
LogicalResult convertToSpirv(gpu::GPUModuleOp module) {
    MLIRContext* context = module.getContext();
    const spirv::TargetEnvAttr env = spirv::lookupTargetEnvOrDefault(module);
    std::unique_ptr<ConversionTarget> target = SPIRVConversionTarget::get(env);
    SPIRVConversionOptions options;
    options.use64bitIndex = true; // index CPU'daki gibi 64 bit
    SPIRVTypeConverter converter(env, options);

    RewritePatternSet patterns(context);
    populateGPUToSPIRVPatterns(converter, patterns);
    ScfToSPIRVContext scfContext;
    populateSCFToSPIRVPatterns(converter, scfContext, patterns);
    arith::populateArithToSPIRVPatterns(converter, patterns);
    populateMathToSPIRVPatterns(converter, patterns);
    populateMemRefToSPIRVPatterns(converter, patterns);
    populateFuncToSPIRVPatterns(converter, patterns);
    // Çekirdeğin çağırdığı cihaz fonksiyonları (func.func → spirv.func);
    // MLIR'in kendi pass'inde yoktur.
    populateBuiltinFuncToSPIRVPatterns(converter, patterns);
    populateVectorToSPIRVPatterns(converter, patterns);
    ub::populateUBToSPIRVConversionPatterns(converter, patterns); // (liftControlFlow'un ub.poison'ları)
    return applyFullConversion(module.getOperation(), *target, std::move(patterns));
}

// SPIR-V'nin istediği yetenekler → çalışma zamanının denetleyeceği bitler.
LogicalResult requirementsOf(spirv::ModuleOp module, uint32_t& requirements, std::string& error) {
    std::optional<spirv::VerCapExtAttr> triple = module.getVceTriple();
    if (!triple) return success();
    for (spirv::Capability capability : triple->getCapabilities()) {
        switch (capability) {
        case spirv::Capability::Shader:
        case spirv::Capability::Matrix:
            break;
        case spirv::Capability::Float64:
            requirements |= MARLIN_GPU_NEEDS_FLOAT64;
            break;
        case spirv::Capability::Int64:
            requirements |= MARLIN_GPU_NEEDS_INT64;
            break;
        default:
            error = "beklenmeyen SPIR-V yetenegi: " + spirv::stringifyCapability(capability).str();
            return failure();
        }
    }
    return success();
}

LogicalResult compileModule(gpu::GPUModuleOp copy, ModuleOp scratch, GpuKernelPackage& package,
                            std::string& error) {
    MLIRContext* context = copy.getContext();
    llvm::SmallVector<gpu::GPUFuncOp, 2> kernels;
    for (auto function : copy.getOps<gpu::GPUFuncOp>())
        if (function.isKernel()) kernels.push_back(function);
    for (gpu::GPUFuncOp kernel : kernels)
        if (failed(prepareKernel(kernel, package, error))) return failure();

    // Çekirdek dışındaki fonksiyonların yerel değişkenleri de Function sınıfına.
    copy.walk([&](memref::AllocaOp alloca) {
        const MemRefType memref = alloca.getType();
        if (memref.getMemorySpace()) return;
        alloca.getResult().setType(MemRefType::get(memref.getShape(), memref.getElementType(), memref.getLayout(),
                                                   spirv::StorageClassAttr::get(context, spirv::StorageClass::Function)));
    });
    copy->setAttr(spirv::getTargetEnvAttrName(), vulkanTarget(context));

    if (failed(liftControlFlow(copy))) {
        error = "yapisiz akis yapisal hale getirilemedi";
        return failure();
    }
    // Sadeleştirme: tek bloğa inen bölgeler (scf.execute_region) açılır;
    // tavana bölme gibi bileşik işlemler açılır.
    PassManager prepare(context, ModuleOp::getOperationName());
    OpPassManager& nested = prepare.nest<gpu::GPUModuleOp>();
    nested.addPass(arith::createArithExpandOpsPass());
    nested.addPass(createCanonicalizerPass());
    if (failed(prepare.run(scratch))) return failure();
    guardEmptyLoops(copy);

    if (failed(convertToSpirv(copy))) {
        if (error.empty()) error = "SPIR-V'ye cevrilemedi";
        return failure();
    }

    // Parametreler → bağlamalar (genel değişkenler), giriş noktası bildirimi;
    // ardından modülün kullandığı yetenekler.
    PassManager finish(context, ModuleOp::getOperationName());
    OpPassManager& spirvPasses = finish.nest<spirv::ModuleOp>();
    spirvPasses.addPass(spirv::createSPIRVLowerABIAttributesPass());
    spirvPasses.addPass(spirv::createSPIRVUpdateVCEPass());
    if (failed(finish.run(scratch))) return failure();

    spirv::ModuleOp result;
    scratch.walk([&](spirv::ModuleOp m) { result = m; });
    if (!result) {
        error = "spirv.module olusmadi";
        return failure();
    }
    if (failed(requirementsOf(result, package.requirements, error))) return failure();
    llvm::SmallVector<uint32_t, 0> words;
    if (failed(spirv::serialize(result, words))) return failure();
    package.spirv.assign(words.begin(), words.end());
    return success();
}

void appendU32(std::string& out, uint32_t value) {
    for (int i = 0; i < 4; ++i) out.push_back(static_cast<char>((value >> (8 * i)) & 0xff));
}

void padTo4(std::string& out) {
    while (out.size() % 4) out.push_back('\0');
}

std::string encodePackage(llvm::StringRef ptx, const GpuKernelPackage& package) {
    std::string ptxText = ptx.str();
    if (!ptxText.empty() && ptxText.back() != '\0') ptxText.push_back('\0');
    std::string table = package.table;
    table.push_back('\0');

    std::string out(MARLIN_GPU_PACKAGE_MAGIC, 8);
    appendU32(out, static_cast<uint32_t>(ptxText.size()));
    appendU32(out, static_cast<uint32_t>(package.spirv.size() * 4));
    appendU32(out, static_cast<uint32_t>(table.size()));
    appendU32(out, 0);
    out += ptxText;
    padTo4(out);
    for (uint32_t word : package.spirv) appendU32(out, word);
    out += table;
    padTo4(out);
    return out;
}

} // namespace

GpuPackages buildGpuPackages(mlir::ModuleOp module, bool noCuda) {
    MLIRContext* context = module.getContext();
    context->getOrLoadDialect<spirv::SPIRVDialect>();
    context->getOrLoadDialect<ub::UBDialect>();
    const bool debug = std::getenv("MARLIN_DEBUG_SPIRV") != nullptr;

    GpuPackages packages;
    for (auto gpuModule : module.getOps<gpu::GPUModuleOp>()) {
        GpuKernelPackage& package = packages[gpuModule.getName().str()];
        if (noCuda) package.requirements |= MARLIN_GPU_NO_CUDA;

        // Kopya üzerinde çalışılır: asıl modül CUDA için olduğu gibi kalır.
        OwningOpRef<ModuleOp> scratch = ModuleOp::create(gpuModule.getLoc());
        OpBuilder builder = OpBuilder::atBlockEnd(scratch->getBody());
        auto copy = mlir::cast<gpu::GPUModuleOp>(builder.clone(*gpuModule));

        // Çevrilemeyen çekirdek kullanıcı hatası değildir: tanılar ekrana
        // değil sebebe yazılır.
        std::string error;
        LogicalResult compiled = failure();
        {
            ScopedDiagnosticHandler capture(context, [&](Diagnostic& diagnostic) {
                if (diagnostic.getSeverity() == DiagnosticSeverity::Error && error.empty())
                    error = diagnostic.str();
                return success();
            });
            compiled = compileModule(copy, *scratch, package, error);
        }
        if (failed(compiled)) {
            package.spirv.clear();
            package.requirements |= MARLIN_GPU_NO_VULKAN;
            package.error = error.empty() ? "bilinmeyen sebep" : error;
        }
        if (debug) {
            llvm::errs() << "SPIR-V " << gpuModule.getName() << ": ";
            if (failed(compiled)) llvm::errs() << "YOK (" << package.error << ")\n";
            else llvm::errs() << package.spirv.size() * 4 << " bayt, gereksinim " << package.requirements << "\n";
            if (std::string(std::getenv("MARLIN_DEBUG_SPIRV")) == "2") scratch->print(llvm::errs());
        }
    }
    return packages;
}

void resolveGpuRequirements(mlir::ModuleOp module, const GpuPackages& packages, uint32_t always) {
    llvm::DenseMap<Operation*, uint32_t> known;
    llvm::DenseSet<Operation*> visiting;
    std::function<uint32_t(func::FuncOp)> requirementsOf = [&](func::FuncOp function) -> uint32_t {
        if (auto it = known.find(function); it != known.end()) return it->second;
        if (!visiting.insert(function).second) return 0; // özyineleme
        uint32_t requirements = 0;
        function.walk([&](Operation* op) {
            if (auto launch = mlir::dyn_cast<gpu::LaunchFuncOp>(op)) {
                auto it = packages.find(launch.getKernelModuleName().str());
                if (it != packages.end()) requirements |= it->second.requirements;
            } else if (auto call = mlir::dyn_cast<func::CallOp>(op)) {
                if (auto callee = module.lookupSymbol<func::FuncOp>(call.getCallee()))
                    requirements |= requirementsOf(callee);
            }
        });
        visiting.erase(function);
        known[function] = requirements;
        return requirements;
    };

    llvm::SmallVector<func::CallOp, 8> decisions;
    module.walk([&](func::CallOp call) {
        if (call->hasAttr("marlin.gpu_entry")) decisions.push_back(call);
    });
    for (func::CallOp call : decisions) {
        auto entry = call->getAttrOfType<FlatSymbolRefAttr>("marlin.gpu_entry");
        uint32_t requirements = always;
        if (auto function = module.lookupSymbol<func::FuncOp>(entry)) requirements |= requirementsOf(function);
        OpBuilder builder(call);
        Value value = builder.create<arith::ConstantIntOp>(call.getLoc(), requirements, 32);
        call->setOperand(0, value);
        call->removeAttr("marlin.gpu_entry");
    }
}

void packGpuBinaries(mlir::ModuleOp module, const GpuPackages& packages) {
    MLIRContext* context = module.getContext();
    auto packaged = [&](Attribute target, llvm::StringRef ptx, const GpuKernelPackage& package,
                        DictionaryAttr properties) -> Attribute {
        return gpu::ObjectAttr::get(context, target, gpu::CompilationTarget::Binary,
                                    StringAttr::get(context, encodePackage(ptx, package)), properties);
    };

    // CUDA ile: gpu-module-to-binary'nin ürettiği PTX pakete konur.
    for (auto binary : module.getOps<gpu::BinaryOp>()) {
        auto it = packages.find(binary.getName().str());
        if (it == packages.end()) continue;
        SmallVector<Attribute, 1> objects;
        for (Attribute attribute : binary.getObjects()) {
            auto object = mlir::cast<gpu::ObjectAttr>(attribute);
            objects.push_back(packaged(object.getTarget(), object.getObject().getValue(), it->second,
                                       object.getProperties()));
        }
        binary.setObjectsAttr(ArrayAttr::get(context, objects));
    }

    // CUDA olmadan: gpu.module doğrudan pakete. (Nesnenin hedef etiketi
    // gömme biçimini etkilemez; paket hangi arka uçları içerdiğini kendi
    // başlığında söyler. NVVM dialect'ini bu yolda başka bir şey yüklemez.)
    context->getOrLoadDialect<NVVM::NVVMDialect>();
    for (auto gpuModule : llvm::make_early_inc_range(module.getOps<gpu::GPUModuleOp>())) {
        auto it = packages.find(gpuModule.getName().str());
        if (it == packages.end()) continue;
        OpBuilder builder(gpuModule);
        Attribute object = packaged(NVVM::NVVMTargetAttr::get(context), "", it->second, nullptr);
        builder.create<gpu::BinaryOp>(gpuModule.getLoc(), gpuModule.getName(), Attribute{},
                                      llvm::ArrayRef<Attribute>{object});
        gpuModule.erase();
    }
}

} // namespace marlin
