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
#
# Çalıştırılan her test hem -O0 hem -O2 ile derlenir.
#
# Kullanım: tests/run.sh [minik_lang yolu]

cd "$(dirname "$0")"
COMPILER="${1:-../cmake-build-debug/minik_lang}"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

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

    for opt in -O0 -O2; do
        if ! "$COMPILER" "$src" "$opt" -o "$TMP/prog" 2> "$TMP/err"; then
            bad "$src $opt" "derlenemedi: $(head -3 "$TMP/err")"; continue
        fi
        "$TMP/prog" > "$TMP/actual" 2> "$TMP/stderr"
        code=$?
        missing=""
        while IFS= read -r msg; do
            grep -qF -- "$msg" "$TMP/stderr" || missing="$missing [$msg]"
        done < <(sed -n 's|^// stderr: *||p' "$src")

        if [ "$code" != "$expected_code" ]; then
            bad "$src $opt" "cikis kodu: beklenen $expected_code, bulunan $code $(head -2 "$TMP/stderr")"
        elif [ -n "$missing" ]; then
            bad "$src $opt" "beklenen hata mesaji yok:$missing"
        elif ! diff -u "$TMP/expected" "$TMP/actual" > "$TMP/diff"; then
            bad "$src $opt" "cikti farkli:"; sed 's/^/           /' "$TMP/diff"
        else
            ok "$src $opt"
        fi
    done
done

echo "$pass gecti, $fail kaldi"
[ "$fail" -eq 0 ]
