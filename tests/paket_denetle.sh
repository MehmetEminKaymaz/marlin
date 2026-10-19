#!/bin/bash
# Kurulu (ya da açılmış) derleyici paketini denetler: paketteki her
# çalıştırılabilir dosyanın ve paylaşımlı kütüphanenin bağımlılıkları ya
# paketin içinden ya da işletim sisteminden gelmeli. Derleme makinesindeki
# bir yola (conda ortamı, LLVM derleme dizini) bağlı kalan dosya, paketi
# başka makinede kullanılamaz yapar.
#
# Kullanım: tests/paket_denetle.sh <paket kökü>   (içinde bin/ ve lib/marlin/)

root="${1:?paket koku verilmeli}"
root="$(cd "$root" && pwd)"
fail=0
bad() { echo "  HATA  $1"; fail=1; }

[ -d "$root/lib/marlin/std" ] || bad "lib/marlin/std yok"
[ -s "$root/THIRD_PARTY_NOTICES.txt" ] || bad "THIRD_PARTY_NOTICES.txt (ucuncu taraf lisanslari) yok"
[ -e "$root/lib/marlin/libmarlinrt.a" ] || [ -e "$root/lib/marlin/marlinrt.lib" ] || bad "calisma zamani kutuphanesi yok"

case "$(uname -s)" in
Linux)
    for f in "$root"/bin/* "$root"/lib/marlin/*.so*; do
        [ -f "$f" ] && [ ! -L "$f" ] || continue
        file -b "$f" | grep -q ELF || continue
        while read -r line; do
            case "$line" in
            *"not found"*) bad "$(basename "$f"): $line" ;;
            *"=> /"*)
                dep="${line#*=> }"; dep="${dep%% (*}"
                case "$dep" in
                "$root"/*|/lib/*|/lib64/*|/usr/lib/*|/usr/lib64/*) ;;
                *) bad "$(basename "$f"): paket disi bagimlilik $dep" ;;
                esac ;;
            esac
        done < <(ldd "$f" 2>&1)
        # Arama yolları (RPATH / RUNPATH) yalnızca pakete göreli olmalı: mutlak
        # bir yol derleme makinesinde var olduğu için ldd'de hata görünmez.
        while read -r entry; do
            case "$entry" in
            '$ORIGIN'*) ;;
            *) bad "$(basename "$f"): paket disi arama yolu $entry" ;;
            esac
        done < <(readelf -d "$f" | sed -n 's/.*(R[UN]*PATH).*\[\(.*\)\]$/\1/p' | tr ':' '\n')
    done
    ;;
Darwin)
    for f in "$root"/bin/* "$root"/lib/marlin/*.dylib; do
        [ -f "$f" ] && [ ! -L "$f" ] || continue
        # Bağımlılıklar: sistem ya da pakette bulunan @rpath / @loader_path adları.
        # (otool -L, kütüphanenin kendi kimliğini de listeler; o atlanır.)
        id="$(otool -D "$f" | tail -n +2)"
        while read -r dep; do
            [ "$dep" = "$id" ] && continue
            case "$dep" in
            /usr/lib/*|/System/*) ;;
            @rpath/*|@loader_path/*|@executable_path/*)
                [ -e "$root/lib/marlin/${dep##*/}" ] || bad "$(basename "$f"): $dep pakette yok" ;;
            *) bad "$(basename "$f"): paket disi bagimlilik $dep" ;;
            esac
        done < <(otool -L "$f" | tail -n +2 | awk '{print $1}')
        # Arama yolları (LC_RPATH) yalnızca pakete göreli olmalı
        while read -r rpath; do
            case "$rpath" in
            @loader_path*|@executable_path*) ;;
            *) bad "$(basename "$f"): paket disi arama yolu $rpath" ;;
            esac
        done < <(otool -l "$f" | awk '/LC_RPATH/ {getline; getline; print $2}')
        codesign --verify "$f" 2> /dev/null || bad "$(basename "$f"): imza gecersiz"
    done
    ;;
MINGW*|MSYS*|CYGWIN*)
    # (dumpbin: Visual Studio'nun aracı; msvc-dev-cmd PATH'e ekler.)
    system="$(cygpath -u "${SYSTEMROOT:-C:/Windows}")/System32"
    for f in "$root"/bin/*.exe "$root"/bin/*.dll; do
        [ -f "$f" ] || continue
        while read -r dep; do
            case "${dep,,}" in api-ms-*|ext-ms-*) continue ;; esac
            [ -e "$root/bin/$dep" ] || [ -e "$system/$dep" ] || bad "$(basename "$f"): $dep pakette ve sistemde yok"
        done < <(dumpbin //nologo //dependents "$(cygpath -w "$f")" | tr -d '\r' |
                 sed -n 's/^ *\([^ ]*\.[dD][lL][lL]\)$/\1/p')
    done
    ;;
esac

if [ "$fail" -ne 0 ]; then
    echo "paket bagimsiz degil"
    exit 1
fi
echo "paket bagimsiz: $(find "$root" -type f | wc -l | tr -d ' ') dosya, $(du -sh "$root" | cut -f1)"
