# Kurulumun son adımı (install(SCRIPT), bkz. CMakeLists.txt "KURULUM VE PAKET").
#
# Paketin başka bir makinede çalışabilmesi için derleyicinin ve programlara
# bağlanan kütüphanelerin, sistemde bulunması beklenmeyen bütün paylaşımlı
# bağımlılıkları pakete kopyalanır (ör. conda'nın libLLVM'i, libzstd'si).
# İşletim sisteminin parçası olanlar kopyalanmaz.
#
# Girdiler (install(CODE) ile verilir):
#   MARLIN_BUNDLE_EXECUTABLES  derleme dizinindeki marlin_lang
#   MARLIN_BUNDLE_LIBRARIES    kendisi de pakete giren kütüphaneler
#                              (MLIR async runtime, MoltenVK)
#   MARLIN_BUNDLE_DIRS         bağımlılıkların aranacağı ek dizinler
#   MARLIN_BUNDLE_DEST         hedef dizin (önekten göreli)

set(dest "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/${MARLIN_BUNDLE_DEST}")

# Kütüphanenin kendisi: bağlantı (symlink) ise içeriği bu adla kopyalanır.
# (Programa yazılan ad sürümlü bağlantının adı olabilir: libX.so.19.1.)
foreach(library IN LISTS MARLIN_BUNDLE_LIBRARIES)
    get_filename_component(name "${library}" NAME)
    file(REAL_PATH "${library}" real)
    message(STATUS "Paket kutuphanesi: ${dest}/${name}")
    file(COPY_FILE "${real}" "${dest}/${name}")
endforeach()

# Sisteme ait olanlar: Linux'ta glibc ve dağıtımın kütüphaneleri, macOS'ta
# /usr/lib ve /System, Windows'ta Windows dizini (MSVC çalışma zamanı da
# oradadır: Visual Studio Build Tools ile gelir).
set(system_paths
    "^/lib/" "^/lib64/" "^/usr/lib/" "^/usr/lib64/"
    "^/System/"
    "^[A-Za-z]:[/\\\\][Ww][Ii][Nn][Dd][Oo][Ww][Ss][/\\\\]")

file(GET_RUNTIME_DEPENDENCIES
    EXECUTABLES ${MARLIN_BUNDLE_EXECUTABLES}
    LIBRARIES ${MARLIN_BUNDLE_LIBRARIES}
    DIRECTORIES ${MARLIN_BUNDLE_DIRS}
    PRE_EXCLUDE_REGEXES "^api-ms-" "^ext-ms-"
    POST_EXCLUDE_REGEXES ${system_paths}
    RESOLVED_DEPENDENCIES_VAR resolved
    UNRESOLVED_DEPENDENCIES_VAR unresolved
    CONFLICTING_DEPENDENCIES_PREFIX conflicting)

if(unresolved)
    message(FATAL_ERROR "Paket: bulunamayan bagimliliklar: ${unresolved}")
endif()
if(conflicting_FILENAMES)
    message(FATAL_ERROR "Paket: ayni adla birden cok kutuphane: ${conflicting_FILENAMES}")
endif()

foreach(dependency IN LISTS resolved)
    message(STATUS "Paket bagimliligi: ${dependency}")
    # (Linux'ta bulunan yol genellikle sürümlü bağlantıdır; zincir korunur.)
    file(INSTALL "${dependency}" DESTINATION "${dest}" FOLLOW_SYMLINK_CHAIN)
endforeach()

# macOS: kurulumda yolları değiştirilen (rpath) dosyaların imzası bozulur;
# Apple Silicon imzasız ya da bozuk imzalı kodu çalıştırmaz. Hepsi geçici
# (ad-hoc) imzayla yeniden imzalanır.
if(CMAKE_HOST_APPLE)
    file(GLOB machO "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/bin/*" "${dest}/*.dylib")
    foreach(file IN LISTS machO)
        if(NOT IS_SYMLINK "${file}")
            execute_process(COMMAND codesign --force --sign - "${file}" RESULT_VARIABLE rc)
            if(NOT rc EQUAL 0)
                message(FATAL_ERROR "codesign basarisiz: ${file}")
            endif()
        endif()
    endforeach()
endif()
