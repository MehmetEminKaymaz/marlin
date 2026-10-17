#include "LLVMLowering.h"

#include "GpuOffload.h"
#include "NestedParallel.h"
#include "ParallelMatmul.h"

#include "llvm/IR/IRBuilder.h"
#include "mlir/Conversion/AffineToStandard/AffineToStandard.h"
#include "mlir/Conversion/ArithToLLVM/ArithToLLVM.h"
#include "mlir/Conversion/AsyncToLLVM/AsyncToLLVM.h"
#include "mlir/Dialect/Arith/Transforms/Passes.h"
#include "mlir/Dialect/Async/Passes.h"
#include "mlir/Conversion/ComplexToLLVM/ComplexToLLVM.h"
#include "mlir/Conversion/ControlFlowToLLVM/ControlFlowToLLVM.h"
#include "mlir/Conversion/FuncToLLVM/ConvertFuncToLLVM.h"
#include "mlir/Conversion/IndexToLLVM/IndexToLLVM.h"
#include "mlir/Conversion/NVVMToLLVM/NVVMToLLVM.h"
#include "mlir/Conversion/UBToLLVM/UBToLLVM.h"
#include "mlir/Conversion/VectorToLLVM/ConvertVectorToLLVMPass.h"
#include "mlir/Conversion/FuncToLLVM/ConvertFuncToLLVMPass.h"
#include "mlir/Conversion/GPUCommon/GPUCommonPass.h"
#include "mlir/Conversion/GPUToNVVM/GPUToNVVMPass.h"
#include "mlir/Conversion/MathToLLVM/MathToLLVM.h"
#include "mlir/Conversion/MathToLibm/MathToLibm.h"
#include "mlir/Conversion/MemRefToLLVM/MemRefToLLVM.h"
#include "mlir/Conversion/ReconcileUnrealizedCasts/ReconcileUnrealizedCasts.h"
#include "mlir/Conversion/SCFToControlFlow/SCFToControlFlow.h"
#include "mlir/Dialect/Arith/Transforms/BufferizableOpInterfaceImpl.h"
#include "mlir/Dialect/Bufferization/Transforms/FuncBufferizableOpInterfaceImpl.h"
#include "mlir/Dialect/Bufferization/Transforms/Passes.h"
#include "mlir/Dialect/ControlFlow/Transforms/BufferizableOpInterfaceImpl.h"
#include "mlir/Dialect/Func/IR/FuncOps.h"
#include "mlir/Dialect/GPU/IR/GPUDialect.h"
#include "mlir/Dialect/LLVMIR/LLVMDialect.h"
#include "mlir/Dialect/GPU/Transforms/Passes.h"
#include "mlir/Dialect/Linalg/Passes.h"
#include "mlir/Dialect/Linalg/Transforms/BufferizableOpInterfaceImpl.h"
#include "mlir/Dialect/MemRef/Transforms/Passes.h"
#include "mlir/Dialect/SCF/Transforms/BufferizableOpInterfaceImpl.h"
#include "mlir/Dialect/Tensor/Transforms/BufferizableOpInterfaceImpl.h"
#include "mlir/Pass/Pass.h"
#include "mlir/Pass/PassManager.h"
#include "mlir/Target/LLVM/NVVM/Target.h"
#include "mlir/Target/LLVMIR/Export.h"
#include "mlir/Transforms/Passes.h"

namespace marlin {

// Bufferization, her operasyonun tensor'larını belleğe nasıl yerleştireceğini
// dialect'lerin sağladığı "dış modeller"den öğrenir. Bunlar MLIRContext
// oluşturulmadan önce registry'ye eklenmelidir.
void registerLoweringInterfaces(mlir::DialectRegistry& registry) {
    mlir::arith::registerBufferizableOpInterfaceExternalModels(registry);
    mlir::cf::registerBufferizableOpInterfaceExternalModels(registry);
    mlir::linalg::registerBufferizableOpInterfaceExternalModels(registry);
    mlir::scf::registerBufferizableOpInterfaceExternalModels(registry);
    mlir::tensor::registerBufferizableOpInterfaceExternalModels(registry);
    mlir::bufferization::func_ext::registerBufferizableOpInterfaceExternalModels(registry);
    // GPU modüllerini NVIDIA makine koduna derleyen hedef.
    mlir::NVVM::registerNVVMTargetInterfaceExternalModels(registry);
    // gpu-to-llvm, CPU tarafındaki kodu llvm dialect'ine çevirirken yüklü her
    // dialect'ten kendi dönüşüm kurallarını ister; kurallar burada kaydedilir.
    mlir::arith::registerConvertArithToLLVMInterface(registry);
    mlir::cf::registerConvertControlFlowToLLVMInterface(registry);
    mlir::registerConvertFuncToLLVMInterface(registry);
    mlir::registerConvertMemRefToLLVMInterface(registry);
    mlir::registerConvertMathToLLVMInterface(registry);
    mlir::registerConvertComplexToLLVMInterface(registry);
    mlir::index::registerConvertIndexToLLVMInterface(registry);
    mlir::ub::registerConvertUBToLLVMInterface(registry);
    mlir::registerConvertNVVMToLLVMInterface(registry);
}

// FONKSİYON ADRESLERİ
//
// Fonksiyon değerleri kod üretiminde i64 adres olarak taşınır; fonksiyon
// tipiyle aralarında unrealized_conversion_cast köprüleri vardır (bkz.
// MLIRGenerator::functionAddress). func → llvm dönüşümü fonksiyon tipini
// !llvm.ptr yapınca köprüler zincir olur:
//
//   değer olarak:  llvm.mlir.addressof → (ptr → fn) → (fn → i64)
//   çağrıda:       i64 → (i64 → fn) → (fn → ptr) → llvm.call
//
// reconcile-unrealized-casts yalnızca başladığı tipe dönen zincirleri
// silebilir; ptr ↔ i64 ise gerçek bir dönüşümdür. Zincirin iki ucu burada
// ptrtoint / inttoptr ile bağlanır, aradaki köprüler kullanılmaz hale gelip
// silinir.
static void lowerFunctionAddressCasts(mlir::ModuleOp module) {
    llvm::SmallVector<mlir::UnrealizedConversionCastOp, 16> casts;
    module.walk([&](mlir::UnrealizedConversionCastOp cast) {
        if (cast->getNumOperands() == 1 && cast->getNumResults() == 1) casts.push_back(cast);
    });

    for (mlir::UnrealizedConversionCastOp cast : casts) {
        mlir::Value origin = cast.getOperand(0);
        while (auto inner = origin.getDefiningOp<mlir::UnrealizedConversionCastOp>()) {
            if (inner->getNumOperands() != 1) break;
            origin = inner.getOperand(0);
        }
        const mlir::Type from = origin.getType();
        const mlir::Type to = cast.getResult(0).getType();
        mlir::OpBuilder builder(cast);
        mlir::Value replacement;
        if (mlir::isa<mlir::LLVM::LLVMPointerType>(from) && to.isInteger(64))
            replacement = builder.create<mlir::LLVM::PtrToIntOp>(cast.getLoc(), to, origin);
        else if (from.isInteger(64) && mlir::isa<mlir::LLVM::LLVMPointerType>(to))
            replacement = builder.create<mlir::LLVM::IntToPtrOp>(cast.getLoc(), to, origin);
        if (replacement) cast.getResult(0).replaceAllUsesWith(replacement);
    }

    // Kullanılmaz kalan köprüler silinir; dıştaki silinince içteki de
    // kullanılmaz hale gelir, bu yüzden değişiklik kalmayana kadar tekrarlanır.
    for (bool erased = true; erased;) {
        erased = false;
        for (mlir::UnrealizedConversionCastOp& cast : casts) {
            if (!cast || !cast.getResult(0).use_empty()) continue;
            cast.erase();
            cast = nullptr;
            erased = true;
        }
    }
}

// GÖREV KÖPRÜLERİ
//
// Görevin değeri (!async.value<T>) runtime'a doğrudan argüman olarak verilir;
// T her sonuç tipi için farklı olduğundan kod üretimi her tip için ayrı adla
// bildirir: marlin_task_wrap.0, marlin_task_wrap.1 ... (bkz.
// MLIRGeneratorTask.cpp). LLVM'e indikten sonra hepsi aynı imzaya sahiptir
// (!llvm.ptr); çağrılar tek C fonksiyonuna (marlin_task_wrap) bağlanır,
// fazla bildirimler silinir.
static void bindTaskBridges(mlir::ModuleOp module) {
    auto baseName = [](llvm::StringRef name) -> std::string {
        if (!name.starts_with("marlin_task_")) return "";
        const size_t dot = name.find('.');
        return dot == llvm::StringRef::npos ? "" : name.take_front(dot).str();
    };
    module.walk([&](mlir::LLVM::CallOp call) {
        std::optional<llvm::StringRef> callee = call.getCallee();
        if (!callee) return;
        const std::string base = baseName(*callee);
        if (!base.empty())
            call.setCalleeAttr(mlir::FlatSymbolRefAttr::get(module.getContext(), base));
    });

    llvm::SmallVector<mlir::LLVM::LLVMFuncOp, 8> bridges;
    for (auto function : module.getOps<mlir::LLVM::LLVMFuncOp>())
        if (!baseName(function.getName()).empty()) bridges.push_back(function);
    for (mlir::LLVM::LLVMFuncOp function : bridges) {
        const std::string base = baseName(function.getName());
        if (module.lookupSymbol(base)) function.erase();
        else function.setSymName(base);
    }
}

// GPU ADIMLARI (yalnızca program @GPU fonksiyon içeriyorsa)
//
// GPU'da çalışacak kod, CPU kodunun arasından adım adım ayrılır:
//
//   G1  marlin-gpu-offload      işaretli fonksiyonlardaki paralel döngüler ve
//                              matris işlemleri → gpu.launch (GpuOffload.h)
//   G2  marlin-gpu-kernel-prepare  sabitleri çekirdeğe taşır, FMA'ya izin verir
//       gpu-kernel-outlining   her gpu.launch gövdesi → ayrı bir gpu.module
//                              içinde gpu.func (çekirdek); yerinde
//                              gpu.launch_func çağrısı kalır. Çekirdeğin
//                              çağırdığı fonksiyonlar da modüle kopyalanır.
//   G3  gpu-async-region       CPU tarafındaki gpu işlemlerini (alloc, memcpy,
//                              launch_func) birbirine sıralı bağlar ve CPU'nun
//                              sonucu okumadan önce GPU'yu beklemesini sağlar.
//   G4  convert-gpu-to-nvvm    gpu.module'ün içi → NVIDIA'ya özgü llvm
//                              dialect'i (matematik → libdevice çağrıları)
//   G5  gpu-to-llvm            CPU tarafındaki gpu işlemleri → GPU çalışma
//                              zamanı çağrıları (libmlir_cuda_runtime.so)
//   G6  gpu-module-to-binary   gpu.module → derlenmiş çekirdek (NVPTX kod
//                              üretici + ptxas); programın içine gömülür.
bool lowerToLLVMDialect(mlir::ModuleOp module, bool usesGpu, bool stopAfterGpuSplit) {
    mlir::PassManager pm(module->getName());
    // Derleyicinin kendisinde hata ayıklamak için: MARLIN_DEBUG_IR tanımlıysa
    // başarısız olan pass'ten sonraki IR stderr'e yazılır.
    if (std::getenv("MARLIN_DEBUG_IR")) {
        module->getContext()->disableMultithreading();
        pm.enableIRPrinting(
            /*shouldPrintBeforePass=*/[](mlir::Pass*, mlir::Operation*) { return false; },
            /*shouldPrintAfterPass=*/[](mlir::Pass*, mlir::Operation*) { return true; },
            /*printModuleScope=*/true, /*printAfterOnlyOnChange=*/false,
            /*printAfterOnlyOnFailure=*/true);
    }

    // A) Matris ifadeleri: tensor → bellek.
    //    Matris işlemleri tensor üzerinde üretilir (bkz. MLIRMatrixManager).
    //    one-shot-bufferize her tensor'u bir belleğe yerleştirir; sonuçlar,
    //    kod üretiminin onlar için ayırdığı belleğe yerinde yazılır.
    //    Tensor seviyesinde çalışacak dönüşümler (döşeme, füzyon ...) bu
    //    adımdan ÖNCE eklenmelidir.
    pm.addPass(mlir::bufferization::createOneShotBufferizePass());

    // G1 — linalg işlemleri hâlâ dururken çalışmalı: @GPU fonksiyonlardakiler
    //      paralel döngüye, oradan GPU çekirdeğine çevrilir.
    if (usesGpu) pm.addPass(createGpuOffloadPass());

    // B) CPU'daki linalg işlemleri → döngüler.
    //    Matris çarpımı döşemeli, vektörlü bir paralel döngüye açılır ve
    //    aşağıda (C) çekirdeklere dağıtılır (bkz. ParallelMatmul.h); kalanlar (eleman eleman işlemler,
    //    fill ...) sıralı scf.for döngüleri olur.
    pm.addPass(createParallelMatmulPass());
    pm.addPass(mlir::createConvertLinalgToLoopsPass());

    // G2
    if (usesGpu) {
        pm.addPass(createGpuKernelPreparePass());
        pm.addPass(mlir::createGpuKernelOutliningPass());
    }
    // (--emit=gpu-mlir burada durur: CPU kodu ve GPU çekirdekleri ayrılmış,
    // ikisi de hâlâ okunabilir üst seviye MLIR.)
    if (stopAfterGpuSplit) return mlir::succeeded(pm.run(module));

    // C) Paralel döngüler (scf.parallel) → iş parçacıkları.
    //    async-parallel-for, yineleme aralığını parçalara böler ve her parçayı
    //    bir görev olarak iş parçacığı havuzuna verir; sonraki pass'ler bu
    //    görevleri çalışma zamanı çağrılarına ve LLVM coroutine'lerine indirir.
    //    (@GPU fonksiyonlardaki paralel döngüler G1'de GPU'ya ayrıldı; buraya
    //    yalnızca CPU'dakiler gelir.)
    //
    //    Ayarlar:
    //      * İş parçacığı sayısı çalışırken makineden öğrenilir (-1); pass'in
    //        varsayılanı sabit 8'dir.
    //      * En küçük parça 1 yinelemedir. Pass'in varsayılanı 1000'dir; o
    //        durumda 1000 yinelemeden kısa döngüler hiç bölünmez ve tek iş
    //        parçacığında çalışır. Kullanıcı `parallel` yazdıysa, yineleme
    //        sayısı az ama her yinelemesi ağır döngüler de dağıtılmalıdır.
    //    Paralel döngünün içinden (iç içe ya da çağrılan fonksiyonda) girilen
    //    paralel döngüler çalışırken sıralı olur; aksi halde havuz işçileri
    //    birbirini bekleyip kilitlenir (bkz. NestedParallel.h).
    pm.addPass(createNestedParallelPass());
    pm.addPass(mlir::createAsyncParallelForPass(/*asyncDispatch=*/true,
                                                /*numWorkerThreads=*/-1,
                                                /*minTaskSize=*/1));
    // Yukarıdaki pass'in ürettiği yardımcı işlemleri (tavana bölme, affine
    // ifadeler) temel işlemlere açar.
    pm.addPass(mlir::arith::createArithExpandOpsPass());
    pm.addPass(mlir::createLowerAffinePass());

    // 0) Listelerin kullandığı üst seviye memref işlemleri daha basit olanlara
    //    açılır: realloc → "yeni yer ayır, kopyala, eskiyi bırak"; görünüm
    //    değiştiren işlemler → açık adres hesabı. Realloc açılımı scf.if
    //    ürettiği için scf dönüşümünden önce gelmelidir.
    pm.addPass(mlir::memref::createExpandReallocPass());
    pm.addPass(mlir::memref::createExpandStridedMetadataPass());
    // Adres hesabı affine ifadeler olarak üretilir; onlar da açılır.
    pm.addPass(mlir::createLowerAffinePass());

    // 1) scf.if / scf.for / scf.while / scf.execute_region → cf blokları.
    //    Bu noktadan sonra yapısal bilgi kaybolur; paralelleştirme ve GPU'ya
    //    yönlendirme gibi scf üzerinde çalışacak pass'ler bundan ÖNCE gelmeli.
    pm.addPass(mlir::createConvertSCFToCFPass());

    // 2) Görevler (async.execute, async.func) → coroutine'ler + çalışma zamanı
    //    çağrıları. `marl` ile başlatılan görevler ve paralel döngülerin
    //    parçaları burada coroutine olur; görevden çağrılan ve bekleyebilen
    //    fonksiyonlar (async.func) da öyle (bkz. MLIRGeneratorTask.cpp).
    //    scf → cf dönüşümünden SONRA gelmelidir: coroutine'in askıya alındığı
    //    nokta yeni bloklar açar ve bu bir scf.if / scf.for bölgesinin içinde
    //    olamaz.
    //    Coroutine'ler ve async tipleri hemen ardından llvm dialect'ine iner.
    //    Araya başka pass girmemeli: desen yeniden yazıcısı kullanan pass'ler
    //    (ör. math-to-libm) aynı görünen blokları birleştirir ve async tipli
    //    blok argümanları açar; async → llvm dönüşümü dal işlenenlerini
    //    çevirmediği için bunlar tutarsız kalırdı.
    pm.addPass(mlir::createAsyncFuncToAsyncRuntimePass());
    pm.addPass(mlir::createAsyncToAsyncRuntimePass());
    pm.addPass(mlir::createAsyncRuntimeRefCountingPass());
    pm.addPass(mlir::createAsyncRuntimeRefCountingOptPass());
    pm.addPass(mlir::createConvertAsyncToLLVMPass());             // görevler → coroutine'ler

    if (usesGpu) {
        // G3 — yalnızca CPU fonksiyonlarında.
        pm.addNestedPass<mlir::func::FuncOp>(mlir::createGpuAsyncRegionPass());
        // G4 — yalnızca GPU modüllerinde. Aşağıdaki matematik dönüşümlerinden
        //      ÖNCE gelmeli: GPU'da sin/cos gibi işlemlerin karşılığı libm
        //      değil libdevice'tır.
        pm.addNestedPass<mlir::gpu::GPUModuleOp>(mlir::createStripDebugInfoPass());
        pm.addNestedPass<mlir::gpu::GPUModuleOp>(mlir::createConvertGpuOpsToNVVMOps());
        pm.addNestedPass<mlir::gpu::GPUModuleOp>(mlir::createReconcileUnrealizedCastsPass());
        // Modüllerin hangi GPU için derleneceği (G6 bunu okur).
        pm.addPass(mlir::createGpuNVVMAttachTarget());
    }

    // 3) Matematik. Önce LLVM'de doğrudan karşılığı (intrinsic) olanlar
    //    çevrilir; kalanlar (tan, atan, atan2, tanh) libm fonksiyonlarına
    //    çağrı olur. Bu çağrılar func dialect'inde üretildiği için func → llvm
    //    dönüşümünden önce gelmelidir.
    pm.addPass(mlir::createConvertMathToLLVMPass());
    pm.addPass(mlir::createConvertMathToLibmPass());

    // 4) Kalan dialect'ler → llvm dialect'i.
    //    Vektör işlemleri (CPU'daki matris çarpımı çekirdeği) memref'lerden
    //    önce çevrilmeli: vektör yükleme/yazma memref'in adres bilgisini okur.
    pm.addPass(mlir::createConvertVectorToLLVMPass());
    pm.addPass(mlir::createFinalizeMemRefToLLVMConversionPass()); // memref → ptr + GEP
    pm.addPass(mlir::createArithToLLVMConversionPass());
    pm.addPass(mlir::createConvertFuncToLLVMPass());
    pm.addPass(mlir::createConvertControlFlowToLLVMPass());

    if (usesGpu) {
        pm.addPass(mlir::createGpuToLLVMConversionPass()); // G5
        pm.addPass(mlir::createGpuModuleToBinaryPass());   // G6
    }
    if (mlir::failed(pm.run(module))) return false;

    // 5) Fonksiyon adreslerinin tip köprüleri → ptrtoint / inttoptr; görevlerin
    //    tipli runtime köprüleri → tek C fonksiyonu.
    lowerFunctionAddressCasts(module);
    bindTaskBridges(module);

    // 6) Dönüşümler arasında geçici olarak bırakılan tip köprülerini
    //    (builtin.unrealized_conversion_cast) temizler.
    mlir::PassManager reconcile(module->getName());
    reconcile.addPass(mlir::createReconcileUnrealizedCastsPass());
    return mlir::succeeded(reconcile.run(module));
}

// Giriş noktası. Kullanıcının `main`i iki durumda sarmalanır; fonksiyon
// içselleştirilir ve onu çağıran bir `i32 main()` eklenir:
//
//   * main void dönüyor: süreç çıkış kodu tanımsız kalırdı; 0 döner.
//   * program görev başlatıyor (`exitThroughRuntime`): main bitince program
//     biter, hâlâ çalışan görevler beklenmez. Sıradan dönüş, kapanışta görev
//     havuzunun kuyruktaki işleri bitirmesini beklerdi; bunun yerine
//     marlin_rt_exit süreci doğrudan kapatır (çıktıları boşaltarak).
static void wrapMain(llvm::Module& module, bool exitThroughRuntime) {
    llvm::Function* userMain = module.getFunction("main");
    if (!userMain) return;
    const bool returnsVoid = userMain->getReturnType()->isVoidTy();
    if (!returnsVoid && !exitThroughRuntime) return;

    userMain->setName("marlin.main");
    userMain->setLinkage(llvm::GlobalValue::InternalLinkage);

    llvm::LLVMContext& context = module.getContext();
    llvm::Type* i32 = llvm::Type::getInt32Ty(context);
    llvm::Function* entry = llvm::Function::Create(
        llvm::FunctionType::get(i32, /*isVarArg=*/false),
        llvm::GlobalValue::ExternalLinkage, "main", module);

    llvm::IRBuilder<> builder(llvm::BasicBlock::Create(context, "entry", entry));
    llvm::Value* call = builder.CreateCall(userMain);
    llvm::Value* code = returnsVoid ? llvm::ConstantInt::get(i32, 0) : call;
    if (exitThroughRuntime) {
        llvm::FunctionCallee exit = module.getOrInsertFunction(
            "marlin_rt_exit", llvm::FunctionType::get(llvm::Type::getVoidTy(context), {i32}, false));
        builder.CreateCall(exit, {code});
        builder.CreateUnreachable();
        return;
    }
    builder.CreateRet(code);
}

std::unique_ptr<llvm::Module> translateToLLVMIR(mlir::ModuleOp module,
                                                llvm::LLVMContext& llvmContext,
                                                bool usesTasks) {
    std::unique_ptr<llvm::Module> llvmModule =
        mlir::translateModuleToLLVMIR(module, llvmContext, "marlin");
    if (!llvmModule) return nullptr;

    wrapMain(*llvmModule, usesTasks);
    return llvmModule;
}

} // namespace marlin
