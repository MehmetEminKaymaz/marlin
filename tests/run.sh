#!/bin/bash
# Testleri çalıştırır (WSL / Linux).
#
# Her .minik dosyası, başındaki yorum satırlarıyla ne beklediğini söyler:
#
#   // output: metin  -> programın yazdığı satırlar, sırasıyla. Çıktının tamamı
#                        bu satırlarla birebir aynı olmalı.
#   // expect: N      -> main'in döndürdüğü değer (sürecin çıkış kodu, 0..255).
#                        Verilmezse 0 beklenir.
#   // stderr: metin  -> program çalışırken hata çıktısına (stderr) bu metni
#                        yazmalı. Çalışma zamanı hatalarını (sınır dışı indeks
#                        gibi) denemek için; genellikle "// expect: 1" ile.
#   // error: metin   -> derleme başarısız olmalı ve hata çıktısı bu metni
#                        içermeli. (Bu satırlardan varsa program çalıştırılmaz.)
#   // leaks: 0       -> program biterken bırakılmamış string kalmamalı.
#                        (Stringlerin belleği kendiliğinden bırakılır; listeler
#                        elle bırakıldığı için test listelerini free etmelidir.)
#
# Çalıştırılan her test hem -O0 hem -O2 ile derlenir.
#
# @GPU fonksiyon içeren testler iki kez çalıştırılır: GPU'da ve --gpu=off ile
# tümüyle CPU'da; ikisi de aynı çıktıyı vermelidir. Makinede kullanılabilir bir
# NVIDIA GPU yoksa yalnızca --gpu=off ile çalıştırılırlar.
#
# Kullanım: tests/run.sh [minik_lang yolu]

cd "$(dirname "$0")"
COMPILER="${1:-../cmake-build-debug/minik_lang}"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

has_gpu=0
if nvidia-smi > /dev/null 2>&1; then has_gpu=1; else echo "  (GPU bulunamadi: @GPU testleri yalnizca --gpu=off ile calisacak)"; fi

pass=0; fail=0
ok()  { echo "  GECTI  $1"; pass=$((pass + 1)); }
bad() { echo "  KALDI  $1: $2"; fail=$((fail + 1)); }

for src in *.minik; do
    if grep -q '^// error:' "$src"; then
        if "$COMPILER" "$src" --emit=mlir > /dev/null 2> "$TMP/err"; then
            bad "$src" "derleme basarili oldu, hata bekleniyordu"; continue
        fi
        missing=""
        while IFS= read -r msg; do
            grep -qF -- "$msg" "$TMP/err" || missing="$missing [$msg]"
        done < <(sed -n 's|^// error: *||p' "$src")
        if [ -n "$missing" ]; then bad "$src" "beklenen hata yok:$missing"; else ok "$src"; fi
        continue
    fi

    expected_code=$(sed -n 's|^// expect: *||p' "$src" | head -1)
    expected_code="${expected_code:-0}"
    # "// output:" (boş satır) ve "// output: metin" biçimlerinin ikisi de geçerli
    sed -n -e 's|^// output: \(.*\)$|\1|p' -e 's|^// output:$||p' "$src" > "$TMP/expected"

    configs=("-O0" "-O2")
    if grep -q '^@GPU' "$src"; then
        configs=("-O0 --gpu=off" "-O2 --gpu=off")
        [ "$has_gpu" = 1 ] && configs+=("-O0" "-O2")
    fi

    for opt in "${configs[@]}"; do
        # ($opt bilerek tırnaksız: birden çok seçenek içerebilir)
        if ! "$COMPILER" "$src" $opt -o "$TMP/prog" 2> "$TMP/err"; then
            bad "$src $opt" "derlenemedi: $(head -3 "$TMP/err")"; continue
        fi
        MINIK_STRING_CHECK=1 "$TMP/prog" > "$TMP/actual" 2> "$TMP/stderr"
        code=$?
        missing=""
        while IFS= read -r msg; do
            grep -qF -- "$msg" "$TMP/stderr" || missing="$missing [$msg]"
        done < <(sed -n 's|^// stderr: *||p' "$src")

        if [ "$code" != "$expected_code" ]; then
            bad "$src $opt" "cikis kodu: beklenen $expected_code, bulunan $code $(head -2 "$TMP/stderr")"
        elif [ -n "$missing" ]; then
            bad "$src $opt" "beklenen hata mesaji yok:$missing"
        elif grep -q '^// leaks: 0' "$src" && grep -q 'string denetimi:' "$TMP/stderr"; then
            bad "$src $opt" "$(grep 'string denetimi:' "$TMP/stderr")"
        elif grep -q "failed with 'CUDA" "$TMP/stderr"; then
            # GPU çalışma zamanı hatayı yalnızca stderr'e yazar, programı durdurmaz.
            bad "$src $opt" "GPU hatasi: $(head -2 "$TMP/stderr")"
        elif ! diff -u "$TMP/expected" "$TMP/actual" > "$TMP/diff"; then
            bad "$src $opt" "cikti farkli:"; sed 's/^/           /' "$TMP/diff"
        else
            ok "$src $opt"
        fi
    done
done

echo "$pass gecti, $fail kaldi"
[ "$fail" -eq 0 ]
