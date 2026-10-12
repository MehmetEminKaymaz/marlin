#include "LLVMLowering.h"

#include "GpuOffload.h"
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

namespace minik {

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

// GPU ADIMLARI (yalnızca program @GPU fonksiyon içeriyorsa)
//
// GPU'da çalışacak kod, CPU kodunun arasından adım adım ayrılır:
//
//   G1  minik-gpu-offload      işaretli fonksiyonlardaki paralel döngüler ve
//                              matris işlemleri → gpu.launch (GpuOffload.h)
//   G2  gpu-kernel-outlining   her gpu.launch gövdesi → ayrı bir gpu.module
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
    //    Matris çarpımı paralel döngüye açılır ve aşağıda (C) çekirdeklere
    //    dağıtılır (bkz. ParallelMatmul.h); kalanlar (eleman eleman işlemler,
    //    fill ...) sıralı scf.for döngüleri olur.
    pm.addPass(createParallelMatmulPass());
    pm.addPass(mlir::createConvertLinalgToLoopsPass());

    // G2
    if (usesGpu) pm.addPass(mlir::createGpuKernelOutliningPass());
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
    pm.addPass(mlir::createAsyncParallelForPass(/*asyncDispatch=*/true,
                                                /*numWorkerThreads=*/-1,
                                                /*minTaskSize=*/1));
    pm.addPass(mlir::createAsyncToAsyncRuntimePass());
    pm.addPass(mlir::createAsyncRuntimeRefCountingPass());
    pm.addPass(mlir::createAsyncRuntimeRefCountingOptPass());
    // Yukarıdaki pass'lerin ürettiği yardımcı işlemleri (tavana bölme, affine
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

    // 2) Matematik. Önce LLVM'de doğrudan karşılığı (intrinsic) olanlar
    //    çevrilir; kalanlar (tan, atan, atan2, tanh) libm fonksiyonlarına
    //    çağrı olur. Bu çağrılar func dialect'inde üretildiği için func → llvm
    //    dönüşümünden önce gelmelidir.
    pm.addPass(mlir::createConvertMathToLLVMPass());
    pm.addPass(mlir::createConvertMathToLibmPass());

    // 3) Kalan dialect'ler → llvm dialect'i.
    pm.addPass(mlir::createFinalizeMemRefToLLVMConversionPass()); // memref → ptr + GEP
    pm.addPass(mlir::createConvertAsyncToLLVMPass());             // görevler → coroutine'ler
    pm.addPass(mlir::createArithToLLVMConversionPass());
    pm.addPass(mlir::createConvertFuncToLLVMPass());
    pm.addPass(mlir::createConvertControlFlowToLLVMPass());

    if (usesGpu) {
        pm.addPass(mlir::createGpuToLLVMConversionPass()); // G5
        pm.addPass(mlir::createGpuModuleToBinaryPass());   // G6
    }

    // 4) Dönüşümler arasında geçici olarak bırakılan tip köprülerini
    //    (builtin.unrealized_conversion_cast) temizler.
    pm.addPass(mlir::createReconcileUnrealizedCastsPass());

    return mlir::succeeded(pm.run(module));
}

// Kullanıcının `main`i void dönüyorsa süreç çıkış kodu tanımsız kalırdı.
// Fonksiyon içselleştirilir ve onu çağırıp 0 dönen bir `i32 main()` eklenir.
static void wrapVoidMain(llvm::Module& module) {
    llvm::Function* userMain = module.getFunction("main");
    if (!userMain || !userMain->getReturnType()->isVoidTy()) return;

    userMain->setName("minik.main");
    userMain->setLinkage(llvm::GlobalValue::InternalLinkage);

    llvm::LLVMContext& context = module.getContext();
    llvm::Type* i32 = llvm::Type::getInt32Ty(context);
    llvm::Function* entry = llvm::Function::Create(
        llvm::FunctionType::get(i32, /*isVarArg=*/false),
        llvm::GlobalValue::ExternalLinkage, "main", module);

    llvm::IRBuilder<> builder(llvm::BasicBlock::Create(context, "entry", entry));
    builder.CreateCall(userMain);
    builder.CreateRet(llvm::ConstantInt::get(i32, 0));
}

std::unique_ptr<llvm::Module> translateToLLVMIR(mlir::ModuleOp module,
                                                llvm::LLVMContext& llvmContext) {
    std::unique_ptr<llvm::Module> llvmModule =
        mlir::translateModuleToLLVMIR(module, llvmContext, "minik");
    if (!llvmModule) return nullptr;

    wrapVoidMain(*llvmModule);
    return llvmModule;
}

} // namespace minik
