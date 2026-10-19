# Kurulumun son adımı (install(SCRIPT), bkz. CMakeLists.txt "KURULUM VE PAKET").
#
# Paketin başka bir makinede çalışabilmesi için derleyicinin ve programlara
# bağlanan kütüphanelerin, sistemde bulunması beklenmeyen bütün paylaşımlı
# bağımlılıkları pakete kopyalanır (ör. conda'nın libLLVM'i, libzstd'si).
# İşletim sisteminin parçası olanlar kopyalanmaz.
#
# Girdiler (install(CODE) ile verilir):
#   MARLIN_BUNDLE_EXECUTABLES  derleme dizinindeki marlin
#   MARLIN_BUNDLE_LIBRARIES    kendisi de pakete giren kütüphaneler (MoltenVK)
#   MARLIN_BUNDLE_DIRS         bağımlılıkların aranacağı ek dizinler
#   MARLIN_BUNDLE_DEST         hedef dizin (önekten göreli)
#   MARLIN_BUNDLE_RPATH_BIN    bin/'den kütüphane dizinine göreli yol (Linux rpath'i)
#   MARLIN_NOTICES_*           lisans bildirimi (bkz. aşağıda ve CMakeLists.txt)

# (Kurulum betikleri projenin politikalarını almaz.)
cmake_policy(VERSION 3.25)

set(dest "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/${MARLIN_BUNDLE_DEST}")

# Kütüphanenin kendisi: bağlantı (symlink) ise içeriği bu adla kopyalanır.
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

# Lisans bildirimi: pakete giren üçüncü taraf parçaların lisansları
# (packaging/third_party_notices.py). Paket derlemesinde metni bulunamayan
# parça hatadır.
if(MARLIN_NOTICES_PYTHON)
    set(strict "")
    if(MARLIN_NOTICES_STRICT)
        set(strict --strict)
    endif()
    execute_process(
        COMMAND "${MARLIN_NOTICES_PYTHON}" "${MARLIN_NOTICES_SCRIPT}"
                --out "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/THIRD_PARTY_NOTICES.txt"
                --bundled ${MARLIN_BUNDLE_LIBRARIES} ${resolved}
                --conda-static ${MARLIN_NOTICES_CONDA_STATIC}
                --extra ${MARLIN_NOTICES_EXTRA}
                ${strict}
        RESULT_VARIABLE rc)
    if(NOT rc EQUAL 0)
        message(FATAL_ERROR "Paket: lisans bildirimi uretilemedi")
    endif()
endif()

# Linux: kütüphaneler yalnızca paketin içinde aranmalı. CMake kurulumda
# derleyicinin rpath'ine araç takımından gelen dizinleri (ör. conda ortamının
# lib'i) bırakabilir, kopyalanan kütüphanelerde de derlendikleri yerin mutlak
# yolları olabilir; o dizin derleme makinesinde bulunduğu sürece hata
# görünmez. Hepsi pakete göreli yazılır. (Rpath'i hiç olmayan dosyaya
# dokunulmaz: CMake var olmayan kaydı ekleyemez.)
if(CMAKE_HOST_UNIX AND NOT CMAKE_HOST_APPLE)
    file(GLOB executables "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/bin/*")
    file(GLOB libraries "${dest}/*.so*")
    foreach(file IN LISTS executables libraries)
        if(IS_SYMLINK "${file}")
            continue()
        endif()
        file(READ_ELF "${file}" RPATH old_rpath RUNPATH old_runpath CAPTURE_ERROR elf_error)
        if(elf_error OR (NOT old_rpath AND NOT old_runpath))
            continue()
        endif()
        if(file IN_LIST executables)
            set(new_rpath "\$ORIGIN/${MARLIN_BUNDLE_RPATH_BIN}")
        else()
            set(new_rpath "\$ORIGIN")
        endif()
        file(RPATH_SET FILE "${file}" NEW_RPATH "${new_rpath}")
    endforeach()
endif()

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
