cmake_minimum_required(VERSION 3.25)
project(MLIRWorkspace)

# Projeyi C++20 standardına ayarlıyoruz
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# LLVM ve MLIR için dizinler
list(APPEND CMAKE_PREFIX_PATH "/home/emin/llvm-project/build-gpu")

find_package(LLVM REQUIRED CONFIG)
find_package(MLIR REQUIRED CONFIG)

message(STATUS "Bulunan LLVM Sürümü: ${LLVM_VERSION}")
message(STATUS "MLIR Paketleri Başarıyla Bağlandı.")

# ==============================================================================
# ANTLR4: MANUEL KÜTÜPHANE ARAMA (pkg-config OLMADAN)
# ==============================================================================

# 1. ANTLR4 Başlık (.h) dosyalarının olduğu klasörü bul
find_path(ANTLR4_INCLUDE_DIR NAMES antlr4-runtime.h
        PATHS /usr/include/antlr4-runtime /usr/local/include/antlr4-runtime
        DOC "ANTLR4 C++ Runtime header dizini")

if(NOT ANTLR4_INCLUDE_DIR)
    message(FATAL_ERROR "antlr4-runtime.h bulunamadi! WSL'de 'sudo apt install libantlr4-runtime-dev' calistirdiginizdan emin olun.")
endif()

# 2. ANTLR4 Derlenmiş Kütüphane (.so veya .a) dosyasını bul
find_library(ANTLR4_LIBRARY NAMES antlr4-runtime
        PATHS /usr/lib /usr/lib/x86_64-linux-gnu /usr/local/lib
        DOC "ANTLR4 C++ Runtime kütüphanesi")

if(NOT ANTLR4_LIBRARY)
    message(FATAL_ERROR "libantlr4-runtime.so bulunamadi!")
endif()

message(STATUS "ANTLR4 Headers: ${ANTLR4_INCLUDE_DIR}")
message(STATUS "ANTLR4 Library: ${ANTLR4_LIBRARY}")

# 3. İndirdiğimiz resmi ANTLR 4.13.1 Jar dosyasının yolu
set(ANTLR_JAR "/home/emin/antlr-4.10-complete.jar")
if(NOT EXISTS "${ANTLR_JAR}")
    message(FATAL_ERROR "ANTLR jar dosyasi bulunamadi! Terminalde calistirin: wget https://www.antlr.org/download/antlr-4.13.1-complete.jar -O /home/emin/antlr-4.13.1-complete.jar")
endif()

# 4. Gramer dosyamızın yolu ve üretilecek kodların kaydedileceği klasör
set(ANTLR_GRAMMAR ${CMAKE_CURRENT_SOURCE_DIR}/minik.g4)
set(ANTLR_OUT_DIR ${CMAKE_CURRENT_BINARY_DIR}/antlr4_cpp)

# 5. ANTLR'ın üreteceği dosyaların listesi
set(ANTLR_GENERATED_SRC
        ${ANTLR_OUT_DIR}/minikLexer.cpp
        ${ANTLR_OUT_DIR}/minikLexer.h
        ${ANTLR_OUT_DIR}/minikParser.cpp
        ${ANTLR_OUT_DIR}/minikParser.h
        ${ANTLR_OUT_DIR}/minikVisitor.cpp
        ${ANTLR_OUT_DIR}/minikVisitor.h
        ${ANTLR_OUT_DIR}/minikBaseVisitor.cpp
        ${ANTLR_OUT_DIR}/minikBaseVisitor.h

)

set(PROJECT_SOURCES
        AST/ASTBuilder.cpp
        AST/ASTBuilder.h
        AST/ASTNode.cpp
        AST/ASTNode.h
        AST/ASTVisitor.cpp
        AST/ASTVisitor.h
        AST/Enums/StatementKind.cpp
        AST/Enums/StatementKind.h
        AST/Enums/ExperessionKind.cpp
        AST/Enums/ExperessionKind.h
        AST/Enums/TypeKind.cpp
        AST/Enums/TypeKind.h
        AST/ASTPrinter.cpp
        AST/ASTPrinter.h
        AST/ModuleLoader.cpp
        AST/ModuleLoader.h
        MLIRBackend/MLIRGenerator.cpp
        MLIRBackend/MLIRGeneratorExpr.cpp
        MLIRBackend/MLIRGeneratorStmt.cpp
        MLIRBackend/MLIRGenerator.h
        MLIRBackend/MLIRControlFlowManager.cpp
        MLIRBackend/MLIRControlFlowManager.h
        MLIRBackend/MLIRMatrixManager.cpp
        MLIRBackend/MLIRMatrixManager.h
        MLIRBackend/MLIRGeneratorMatrix.cpp
        MLIRBackend/MLIRGeneratorStruct.cpp
        MLIRBackend/MLIRGeneratorList.cpp
        MLIRBackend/MLIRGeneratorString.cpp
        MLIRBackend/MLIRListManager.cpp
        MLIRBackend/MLIRListManager.h
        MLIRBackend/MLIRRuntime.h
        MLIRBackend/MLIRBuiltinManager.cpp
        MLIRBackend/MLIRBuiltinManager.h
        MLIRBackend/MLIRArithManager.cpp
        MLIRBackend/MLIRArithManager.h
        MLIRBackend/MLIRScopeManager.cpp
        MLIRBackend/MLIRScopeManager.h
        MLIRBackend/MLIRTypeManager.cpp
        MLIRBackend/MLIRTypeManager.h
        MLIRBackend/CodegenContext.cpp
        MLIRBackend/CodegenContext.h
        LLVMBackend/LLVMLowering.cpp
        LLVMBackend/LLVMLowering.h
        LLVMBackend/NativeCodegen.cpp
        LLVMBackend/NativeCodegen.h
        LLVMBackend/GpuOffload.cpp
        LLVMBackend/GpuOffload.h
        LLVMBackend/ParallelMatmul.cpp
        LLVMBackend/ParallelMatmul.h
        AST/Builtins.h
        AST/SemType.h
        AST/Sema.cpp
        AST/SemaParallel.cpp
        AST/SemaGpu.cpp
        AST/SemaStruct.cpp
        AST/Sema.h
)

# 6. CMake'e bu dosyaları arka planda nasıl üreteceğini öğretiyoruz
add_custom_command(
        OUTPUT ${ANTLR_GENERATED_SRC}
        COMMAND java -jar ${ANTLR_JAR} -Dlanguage=Cpp -visitor -no-listener -o ${ANTLR_OUT_DIR} ${ANTLR_GRAMMAR}
        DEPENDS ${ANTLR_GRAMMAR}
        COMMENT "ANTLR: minik.g4 dosyasindan C++ kodlari uretiliyor..."
)
# ==============================================================================

include_directories(${LLVM_INCLUDE_DIRS} ${MLIR_INCLUDE_DIRS})

# "native": derleyicinin çalıştığı makinenin hedefi (kod üretici, asm yazıcı ...)
llvm_map_components_to_libnames(LLVM_NATIVE_LIBS native passes target mc)
add_definitions(${LLVM_DEFINITIONS})

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# 7. Üretilen AST (cpp ve h) dosyalarını main.cpp ile birlikte derlemeye ekle
add_executable(minik_lang
        main.cpp
        ${ANTLR_GENERATED_SRC}
        ${PROJECT_SOURCES}
)

# LLVM/MLIR, RTTI kapalı derlenmiştir. Kendi pass'imizi tanımlayan dosya
# (mlir::Pass'ten türeyen sınıf) da öyle derlenmeli; aksi halde bağlayıcı
# mlir::Pass'in tip bilgisini arar ve bulamaz. Projenin geri kalanı RTTI
# kullanır (AST üzerinde dynamic_cast, ANTLR).
set_source_files_properties(LLVMBackend/GpuOffload.cpp LLVMBackend/ParallelMatmul.cpp
        PROPERTIES COMPILE_OPTIONS "-fno-rtti")

# Çalışma zamanı kütüphanesi: derlenen her minik programına bağlanır (print vb.).
# Derleyiciyle aynı dizine üretilir; minik_lang onu yanında arar, bulamazsa
# aşağıda gömülen yola bakar.
# std_*.c: standart kütüphane modüllerinin (std/*.minik) C ile yazılmış kısmı.
add_library(minikrt STATIC
        runtime/minik_rt.c
        runtime/minik_rt.h
        runtime/std_string.c
        runtime/std_file.c
        runtime/std_io.c
        runtime/std_os.c
        runtime/std_time.c
        runtime/std_random.c)
add_dependencies(minik_lang minikrt)
target_compile_definitions(minik_lang PRIVATE MINIK_RUNTIME_LIB="$<TARGET_FILE:minikrt>")

# Standart kütüphanenin minik dosyaları (import std.file → std/file.minik).
# minik_lang önce yanındaki `std` dizinine, sonra buraya bakar.
target_compile_definitions(minik_lang PRIVATE MINIK_STD_DIR="${CMAKE_CURRENT_SOURCE_DIR}/std")

# Paralel döngü içeren programlar MLIR'in iş parçacığı havuzuna
# (libmlir_async_runtime.so) bağlanır; minik_lang onu bu dizinde arar.
target_compile_definitions(minik_lang PRIVATE MINIK_LLVM_LIB_DIR="${LLVM_LIBRARY_DIR}")

# 8. ANTLR'ın ürettiği dosyaların ve bulduğumuz sistem başlıklarının yolunu projeye tanıt
target_include_directories(minik_lang PRIVATE
        ${ANTLR_OUT_DIR}
        ${ANTLR4_INCLUDE_DIR}
)

# 9. MLIR kütüphanelerini ve bulduğumuz ANTLR kütüphanesini (SO/A dosyası) bağla
target_link_libraries(minik_lang PRIVATE
        MLIRIR
        MLIRParser
        MLIRSupport
        MLIRDialect

        LLVMCore
        LLVMSupport
        LLVMAsmParser
        LLVMIRReader
        LLVMTransformUtils

        MLIRFuncDialect
        MLIRArithDialect
        MLIRMathDialect
        MLIRSCFDialect
        MLIRControlFlowDialect
        MLIRMemRefDialect
        MLIRLLVMDialect

        MLIRToLLVMIRTranslationRegistration
        MLIRFuncToLLVM
        MLIRArithToLLVM
        MLIRMathToLLVM
        MLIRMathToLibm
        MLIRMemRefTransforms

        # Matrisler: tensor üzerinde linalg, bufferization ile belleğe yerleştirme
        MLIRTensorDialect
        MLIRLinalgDialect
        MLIRBufferizationDialect
        MLIRBufferizationTransforms
        MLIRLinalgTransforms
        MLIRTensorTransforms
        MLIRArithTransforms
        MLIRSCFTransforms
        MLIRControlFlowTransforms

        # Paralel döngüler: scf.parallel → async → coroutine'ler
        MLIRAsyncDialect
        MLIRAsyncTransforms
        MLIRAsyncToLLVM
        MLIRAffineDialect
        MLIRAffineToStandard
        MLIRMemRefToLLVM
        MLIRSCFToControlFlow
        MLIRControlFlowToLLVM
        MLIRReconcileUnrealizedCasts

        # GPU: gpu dialect'i, çekirdeklerin ayrılması, NVIDIA (NVVM) hedefi.
        # NVPTX kod üreticisi MLIRNVVMTarget üzerinden gelir.
        MLIRGPUDialect
        MLIRGPUTransforms
        MLIRGPUToNVVMTransforms
        MLIRGPUToGPURuntimeTransforms
        MLIRNVVMDialect
        MLIRNVVMTarget
        MLIRNVVMToLLVM
        MLIRUBToLLVM
        MLIRIndexToLLVM
        MLIRComplexToLLVM

        # LLVM IR -> makine kodu: yerel hedef (X86) + optimizasyon hattı
        ${LLVM_NATIVE_LIBS}

        MLIRPass
        MLIRTransforms

        ${ANTLR4_LIBRARY}
)