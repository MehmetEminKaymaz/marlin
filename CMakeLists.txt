cmake_minimum_required(VERSION 3.25)
project(MLIRWorkspace)

# Projeyi C++20 standardına ayarlıyoruz
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# LLVM ve MLIR için dizinler. Başka bir makinede -DMARLIN_LLVM_BUILD=<dizin>
# (ya da doğrudan -DMLIR_DIR=...) ile verilir.
set(MARLIN_LLVM_BUILD "/home/emin/llvm-project/build-gpu" CACHE PATH "LLVM/MLIR derleme ya da kurulum dizini")
list(APPEND CMAKE_PREFIX_PATH "${MARLIN_LLVM_BUILD}")

find_package(LLVM REQUIRED CONFIG)
find_package(MLIR REQUIRED CONFIG)

message(STATUS "Bulunan LLVM Sürümü: ${LLVM_VERSION}")
message(STATUS "MLIR Paketleri Başarıyla Bağlandı.")

# ==============================================================================
# ANTLR4: MANUEL KÜTÜPHANE ARAMA (pkg-config OLMADAN)
# ==============================================================================

# İki yol var:
#   * Sistemdeki runtime (Linux: apt libantlr4-runtime-dev, macOS: Homebrew)
#     ve aynı sürümün jar dosyası (ANTLR_JAR).
#   * -DMARLIN_ANTLR_FROM_SOURCE=ON: runtime antlr.org'dan indirilip projeyle
#     birlikte statik derlenir, aynı sürümün jar'ı da indirilir. Paketi
#     olmayan platformlar (Windows) ve CI böyle derler.
# Üretici (jar) ile runtime aynı sürümden olmalıdır.
option(MARLIN_ANTLR_FROM_SOURCE "ANTLR C++ runtime'ini kaynaktan derle" OFF)
set(MARLIN_ANTLR_VERSION "4.13.2" CACHE STRING "Kaynaktan derlenen ANTLR surumu")

if(MARLIN_ANTLR_FROM_SOURCE)
    include(FetchContent)
    set(ANTLR_BUILD_CPP_TESTS OFF CACHE BOOL "" FORCE)
    set(ANTLR_BUILD_SHARED OFF CACHE BOOL "" FORCE)
    set(ANTLR_BUILD_STATIC ON CACHE BOOL "" FORCE)
    # MSVC: C kütüphanesinin DLL hali (/MD); LLVM de öyle derlenmiştir.
    set(WITH_STATIC_CRT OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(antlr4_runtime
            URL https://www.antlr.org/download/antlr4-cpp-runtime-${MARLIN_ANTLR_VERSION}-source.zip
            DOWNLOAD_EXTRACT_TIMESTAMP ON)
    FetchContent_MakeAvailable(antlr4_runtime)
    # Başkasının kodu: uyarıları bizim derleme çıktımızı boğmasın.
    # ANTLR 4.13.2'de ProfilingATNSimulator.cpp <chrono>'yu içermeyi unutur;
    # GCC/Clang'ın standart kütüphanesinde başka başlıktan gelir, MSVC'ninkinde gelmez.
    if(MSVC)
        target_compile_options(antlr4_static PRIVATE /w /FIchrono)
    else()
        target_compile_options(antlr4_static PRIVATE -w)
    endif()
    set(ANTLR4_INCLUDE_DIR ${antlr4_runtime_SOURCE_DIR}/runtime/src)
    set(ANTLR4_LIBRARY antlr4_static)

    set(ANTLR_JAR "${CMAKE_CURRENT_BINARY_DIR}/antlr-${MARLIN_ANTLR_VERSION}-complete.jar")
    if(NOT EXISTS "${ANTLR_JAR}")
        file(DOWNLOAD https://www.antlr.org/download/antlr-${MARLIN_ANTLR_VERSION}-complete.jar
                "${ANTLR_JAR}" STATUS antlrJarStatus)
        list(GET antlrJarStatus 0 antlrJarCode)
        if(NOT antlrJarCode EQUAL 0)
            message(FATAL_ERROR "ANTLR jar dosyasi indirilemedi: ${antlrJarStatus}")
        endif()
    endif()
else()
    # 1. ANTLR4 Başlık (.h) dosyalarının olduğu klasörü bul
    find_path(ANTLR4_INCLUDE_DIR NAMES antlr4-runtime.h
            PATH_SUFFIXES antlr4-runtime
            PATHS /usr/include /usr/local/include /opt/homebrew/include
            DOC "ANTLR4 C++ Runtime header dizini")
    if(NOT ANTLR4_INCLUDE_DIR)
        message(FATAL_ERROR "antlr4-runtime.h bulunamadi! 'sudo apt install libantlr4-runtime-dev' "
                            "ya da -DMARLIN_ANTLR_FROM_SOURCE=ON")
    endif()

    # 2. ANTLR4 Derlenmiş Kütüphane (.so veya .a) dosyasını bul
    find_library(ANTLR4_LIBRARY NAMES antlr4-runtime
            PATHS /usr/lib /usr/lib/x86_64-linux-gnu /usr/local/lib /opt/homebrew/lib
            DOC "ANTLR4 C++ Runtime kütüphanesi")
    if(NOT ANTLR4_LIBRARY)
        message(FATAL_ERROR "libantlr4-runtime bulunamadi! (ya da -DMARLIN_ANTLR_FROM_SOURCE=ON)")
    endif()

    # 3. Runtime ile aynı sürümün jar dosyası
    set(ANTLR_JAR "/home/emin/antlr-4.10-complete.jar" CACHE FILEPATH "ANTLR arac jar dosyasi")
    if(NOT EXISTS "${ANTLR_JAR}")
        message(FATAL_ERROR "ANTLR jar dosyasi bulunamadi (${ANTLR_JAR}); -DANTLR_JAR=<yol> ile verin "
                            "(surumu runtime ile ayni olmali)")
    endif()
endif()

message(STATUS "ANTLR4 Headers: ${ANTLR4_INCLUDE_DIR}")
message(STATUS "ANTLR4 Library: ${ANTLR4_LIBRARY}")
message(STATUS "ANTLR4 Jar: ${ANTLR_JAR}")

# 4. Gramer dosyamızın yolu ve üretilecek kodların kaydedileceği klasör
set(ANTLR_GRAMMAR ${CMAKE_CURRENT_SOURCE_DIR}/marlin.g4)
set(ANTLR_OUT_DIR ${CMAKE_CURRENT_BINARY_DIR}/antlr4_cpp)

# 5. ANTLR'ın üreteceği dosyaların listesi
set(ANTLR_GENERATED_SRC
        ${ANTLR_OUT_DIR}/marlinLexer.cpp
        ${ANTLR_OUT_DIR}/marlinLexer.h
        ${ANTLR_OUT_DIR}/marlinParser.cpp
        ${ANTLR_OUT_DIR}/marlinParser.h
        ${ANTLR_OUT_DIR}/marlinVisitor.cpp
        ${ANTLR_OUT_DIR}/marlinVisitor.h
        ${ANTLR_OUT_DIR}/marlinBaseVisitor.cpp
        ${ANTLR_OUT_DIR}/marlinBaseVisitor.h

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
        MLIRBackend/MLIRGeneratorTask.cpp
        MLIRBackend/MLIRGeneratorScan.cpp
        MLIRBackend/MLIRGeneratorMap.cpp
        MLIRBackend/MLIRGeneratorJson.cpp
        Lsp/MarlinLsp.cpp
        Lsp/MarlinLsp.h
        Lsp/MarlinCheck.cpp
        Lsp/MarlinCheck.h
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
        LLVMBackend/NestedParallel.cpp
        LLVMBackend/NestedParallel.h
        AST/Builtins.h
        AST/SemType.h
        AST/Sema.cpp
        AST/SemaParallel.cpp
        AST/SemaGpu.cpp
        AST/SemaStruct.cpp
        AST/SemaTask.cpp
        AST/SemaBorrow.cpp
        AST/Sema.h
)

# 6. CMake'e bu dosyaları arka planda nasıl üreteceğini öğretiyoruz
add_custom_command(
        OUTPUT ${ANTLR_GENERATED_SRC}
        COMMAND java -jar ${ANTLR_JAR} -Dlanguage=Cpp -visitor -no-listener -o ${ANTLR_OUT_DIR} ${ANTLR_GRAMMAR}
        DEPENDS ${ANTLR_GRAMMAR}
        COMMENT "ANTLR: marlin.g4 dosyasindan C++ kodlari uretiliyor..."
)
# ==============================================================================

include_directories(${LLVM_INCLUDE_DIRS} ${MLIR_INCLUDE_DIRS})

# LLVM'in kendisi. İki biçimde kurulmuş olabilir:
#   * bileşen bileşen statik kütüphaneler (kendi derlememiz): gerekenler adıyla
#     bağlanır; "native" derleyicinin çalıştığı makinenin hedefidir (kod
#     üretici, asm yazıcı ...);
#   * tek paylaşımlı kütüphane, libLLVM (LLVM_LINK_LLVM_DYLIB; ör. conda-forge):
#     MLIR kütüphaneleri de ona bağlıdır. Yanına statik bileşen bağlanırsa
#     programda LLVM'in iki kopyası olur (kayıt tabloları ikiye bölünür,
#     derleyici çöker); bu yüzden yalnızca o bağlanır.
if(LLVM_LINK_LLVM_DYLIB)
    set(MARLIN_LLVM_LIBS LLVM)
else()
    llvm_map_components_to_libnames(LLVM_NATIVE_LIBS native passes target mc)
    set(MARLIN_LLVM_LIBS LLVMCore LLVMSupport LLVMAsmParser LLVMIRReader LLVMTransformUtils ${LLVM_NATIVE_LIBS})
endif()
message(STATUS "LLVM kutuphaneleri: ${MARLIN_LLVM_LIBS}")
add_definitions(${LLVM_DEFINITIONS})

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# 7. Üretilen AST (cpp ve h) dosyalarını main.cpp ile birlikte derlemeye ekle
add_executable(marlin_lang
        main.cpp
        ${ANTLR_GENERATED_SRC}
        ${PROJECT_SOURCES}
)

# LLVM/MLIR, RTTI kapalı derlenmiştir. Kendi pass'imizi tanımlayan dosya
# (mlir::Pass'ten türeyen sınıf) da öyle derlenmeli; aksi halde bağlayıcı
# mlir::Pass'in tip bilgisini arar ve bulamaz. Projenin geri kalanı RTTI
# kullanır (AST üzerinde dynamic_cast, ANTLR).
set(MARLIN_NO_RTTI "$<IF:$<CXX_COMPILER_ID:MSVC>,/GR-,-fno-rtti>")
set_source_files_properties(LLVMBackend/GpuOffload.cpp LLVMBackend/ParallelMatmul.cpp
        LLVMBackend/NestedParallel.cpp PROPERTIES COMPILE_OPTIONS "${MARLIN_NO_RTTI}")
# Dil sunucusu da MLIR'in LSP kütüphanesinin sınıflarını (llvm::ErrorInfo ...)
# kullanır; aynı sebeple RTTI'siz derlenir. AST'ye dokunan kısmı ayrı bir
# dosyadadır (Lsp/MarlinCheck.cpp).
set_source_files_properties(Lsp/MarlinLsp.cpp PROPERTIES COMPILE_OPTIONS "${MARLIN_NO_RTTI}")

# Çalışma zamanı kütüphanesi: derlenen her marlin programına bağlanır (print vb.).
# Derleyiciyle aynı dizine üretilir; marlin_lang onu yanında arar, bulamazsa
# aşağıda gömülen yola bakar.
# std_*.c: standart kütüphane modüllerinin (std/*.marl) C ile yazılmış kısmı.
# platform_*.c: işletim sistemine dokunan her şey (platform.h); her dosya
# yalnızca kendi platformunda derlenir (#ifdef), liste her yerde aynıdır.
add_library(marlinrt STATIC
        runtime/marlin_rt.c
        runtime/marlin_rt.h
        runtime/platform.h
        runtime/platform_posix.c
        runtime/platform_win32.c
        runtime/std_string.c
        runtime/std_file.c
        runtime/std_io.c
        runtime/std_os.c
        runtime/std_time.c
        runtime/std_random.c
        runtime/std_http.c
        runtime/http_parser.c
        runtime/http_parser.h
        runtime/marlin_task.c
        runtime/marlin_map.c
        runtime/marlin_gpu.c
        runtime/std_json.c
        runtime/json_parser.c
        runtime/json_parser.h)
add_dependencies(marlin_lang marlinrt)
target_compile_definitions(marlin_lang PRIVATE MARLIN_RUNTIME_LIB="$<TARGET_FILE:marlinrt>")

# Veritabanı sürücüleri (isteğe bağlı). Kütüphanesi bulunan sürücü derlenir;
# bulunmayanı import eden program açık bir hatayla durur. Önce sistem paketleri
# (libpq-dev, libsqlite3-dev), sonra MARLIN_DEPS_DIR altına yönetici izni
# olmadan açılmış paketler aranır (apt-get download + dpkg -x).
set(MARLIN_DEPS_DIR "$ENV{HOME}/marlin-deps/root/usr" CACHE PATH
    "Sisteme kurulmadan acilmis kutuphaneler (libpq, sqlite)")
find_path(MARLIN_PQ_INCLUDE libpq-fe.h PATH_SUFFIXES postgresql PATHS ${MARLIN_DEPS_DIR}/include)
find_library(MARLIN_PQ_LIB NAMES pq libpq PATHS ${MARLIN_DEPS_DIR}/lib/x86_64-linux-gnu)
if(MARLIN_PQ_INCLUDE AND MARLIN_PQ_LIB)
    message(STATUS "std.postgres: libpq ${MARLIN_PQ_LIB}")
    target_sources(marlinrt PRIVATE runtime/std_postgres.c)
    set_source_files_properties(runtime/std_postgres.c PROPERTIES INCLUDE_DIRECTORIES "${MARLIN_PQ_INCLUDE}")
    target_compile_definitions(marlin_lang PRIVATE MARLIN_PQ_LIB="${MARLIN_PQ_LIB}")
else()
    message(STATUS "std.postgres: libpq bulunamadi (libpq-dev); surucu derlenmeyecek")
endif()
find_path(MARLIN_SQLITE_INCLUDE sqlite3.h PATHS ${MARLIN_DEPS_DIR}/include)
find_library(MARLIN_SQLITE_LIB NAMES sqlite3 libsqlite3.so.0 PATHS ${MARLIN_DEPS_DIR}/lib/x86_64-linux-gnu)
if(MARLIN_SQLITE_INCLUDE AND MARLIN_SQLITE_LIB)
    message(STATUS "std.sqlite: ${MARLIN_SQLITE_LIB}")
    target_sources(marlinrt PRIVATE runtime/std_sqlite.c)
    set_source_files_properties(runtime/std_sqlite.c PROPERTIES INCLUDE_DIRECTORIES "${MARLIN_SQLITE_INCLUDE}")
    target_compile_definitions(marlin_lang PRIVATE MARLIN_SQLITE_LIB="${MARLIN_SQLITE_LIB}")
else()
    message(STATUS "std.sqlite: sqlite3 bulunamadi (libsqlite3-dev); surucu derlenmeyecek")
endif()

# HTTP istemcisi (std.http.client): libcurl. (Debian/Ubuntu'da başlıklar
# çok mimarili dizindedir: include/x86_64-linux-gnu/curl.)
find_path(MARLIN_CURL_INCLUDE curl/curl.h
        PATHS ${MARLIN_DEPS_DIR}/include ${MARLIN_DEPS_DIR}/include/x86_64-linux-gnu)
find_library(MARLIN_CURL_LIB NAMES curl libcurl libcurl_imp PATHS ${MARLIN_DEPS_DIR}/lib/x86_64-linux-gnu)
if(MARLIN_CURL_INCLUDE AND MARLIN_CURL_LIB)
    message(STATUS "std.http.client: libcurl ${MARLIN_CURL_LIB}")
    target_sources(marlinrt PRIVATE runtime/std_http_client.c)
    set_source_files_properties(runtime/std_http_client.c PROPERTIES INCLUDE_DIRECTORIES "${MARLIN_CURL_INCLUDE}")
    target_compile_definitions(marlin_lang PRIVATE MARLIN_CURL_LIB="${MARLIN_CURL_LIB}")
else()
    message(STATUS "std.http.client: libcurl bulunamadi (libcurl4-openssl-dev); istemci derlenmeyecek")
endif()

# Standart kütüphanenin marlin dosyaları (import std.file → std/file.marl).
# marlin_lang önce yanındaki `std` dizinine, sonra buraya bakar.
target_compile_definitions(marlin_lang PRIVATE MARLIN_STD_DIR="${CMAKE_CURRENT_SOURCE_DIR}/std")

# Paralel döngü içeren programlar MLIR'in iş parçacığı havuzuna
# (libmlir_async_runtime.so) bağlanır; marlin_lang onu bu dizinde arar.
# Windows'ta DLL'ler bin dizinindedir; program derlenince yanına kopyalanır.
target_compile_definitions(marlin_lang PRIVATE MARLIN_LLVM_LIB_DIR="${LLVM_LIBRARY_DIR}"
        MARLIN_LLVM_BIN_DIR="${LLVM_TOOLS_BINARY_DIR}")

# 8. ANTLR'ın ürettiği dosyaların ve bulduğumuz sistem başlıklarının yolunu projeye tanıt
target_include_directories(marlin_lang PRIVATE
        ${ANTLR_OUT_DIR}
        ${ANTLR4_INCLUDE_DIR}
)
if(MARLIN_ANTLR_FROM_SOURCE)
    target_compile_definitions(marlin_lang PRIVATE ANTLR4CPP_STATIC)
endif()

# 9. MLIR kütüphanelerini ve bulduğumuz ANTLR kütüphanesini (SO/A dosyası) bağla
target_link_libraries(marlin_lang PRIVATE
        MLIRIR
        MLIRParser
        MLIRSupport
        MLIRDialect

        ${MARLIN_LLVM_LIBS}

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

        # Dil sunucusu (marlin_lang --lsp): MLIR'in LSP altyapısı
        MLIRLspServerSupportLib

        # CPU'da matris çarpımı çekirdeği: vektör işlemleri → llvm
        MLIRVectorDialect
        MLIRVectorToLLVMPass

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

        MLIRPass
        MLIRTransforms

        ${ANTLR4_LIBRARY}
)