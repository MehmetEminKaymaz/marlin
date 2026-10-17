#!/bin/bash
# Testleri çalıştırır (Linux, macOS; Windows'ta Git Bash ile).
#
# Her .marl dosyası, başındaki yorum satırlarıyla ne beklediğini söyler:
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
# HTTP sunucusu testleri (std.http):
#
#   // http: PORT     -> program bir sunucudur: arka planda başlatılır ve
#                        PORT'a aşağıdaki istekler sırayla gönderilir.
#                        Program, beklediği istekleri işleyince kendisi bitmeli.
#   // send: istek    -> ham istek; \r\n gibi kaçış dizileri yazılabilir
#                        (bkz. http_gonder.py).
#   // reply: metin   -> sunucunun yanıtları, sırasıyla ve birleştirilmiş
#                        olarak (satır sonları \n'e çevrilir). Tamamı birebir
#                        aynı olmalı.
#
# Dış bir sunucu gerektiren testler:
#
#   // requires: postgres -> PostgreSQL sunucusu gerekir. Bağlantı bilgisi
#                        MARLIN_PG_TEST ortam değişkenindedir (ör.
#                        "host=localhost port=5432 dbname=test user=ad password=..").
#                        Tanımlı değilse test atlanır.
#
# Çalıştırılan her test hem -O0 hem -O2 ile derlenir.
#
# @GPU fonksiyon içeren testler iki kez çalıştırılır: GPU'da ve --gpu=off ile
# tümüyle CPU'da; ikisi de aynı çıktıyı vermelidir. Makinede kullanılabilir bir
# NVIDIA GPU yoksa yalnızca --gpu=off ile çalıştırılırlar.
#
# Kullanım: tests/run.sh [marlin_lang yolu]

cd "$(dirname "$0")"
COMPILER="${1:-../cmake-build-debug/marlin_lang}"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

# Platform farkları: Windows'ta çalıştırılabilir dosya .exe uzantılıdır ve
# programın çıktısı \r\n satır sonludur; Python'ın adı python olabilir;
# macOS'ta timeout yoktur (Homebrew coreutils: gtimeout).
EXE=""
case "$(uname -s)" in MINGW*|MSYS*|CYGWIN*) EXE=".exe" ;; esac
PY=python3
command -v python3 > /dev/null 2>&1 && python3 -c '' 2> /dev/null || PY=python
TIMEOUT=""
if command -v timeout > /dev/null 2>&1; then TIMEOUT="timeout 60"
elif command -v gtimeout > /dev/null 2>&1; then TIMEOUT="gtimeout 60"; fi
PROG="$TMP/prog$EXE"

has_gpu=0
if nvidia-smi > /dev/null 2>&1; then has_gpu=1; else echo "  (GPU bulunamadi: @GPU testleri yalnizca --gpu=off ile calisacak)"; fi

pass=0; fail=0; skipped=0
ok()  { echo "  GECTI  $1"; pass=$((pass + 1)); }
bad() { echo "  KALDI  $1: $2"; fail=$((fail + 1)); }

for src in *.marl; do
    if grep -q '^// requires: postgres' "$src" && [ -z "$MARLIN_PG_TEST" ]; then
        echo "  ATLANDI  $src (MARLIN_PG_TEST tanimli degil)"; skipped=$((skipped + 1)); continue
    fi
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
    port=$(sed -n 's|^// http: *||p' "$src" | head -1)
    sed -n -e 's|^// reply: \(.*\)$|\1|p' -e 's|^// reply:$||p' "$src" > "$TMP/expected_replies"

    configs=("-O0" "-O2")
    if grep -q '^@GPU' "$src"; then
        configs=("-O0 --gpu=off" "-O2 --gpu=off")
        [ "$has_gpu" = 1 ] && configs+=("-O0" "-O2")
    fi

    for opt in "${configs[@]}"; do
        # ($opt bilerek tırnaksız: birden çok seçenek içerebilir)
        $TIMEOUT "$COMPILER" "$src" $opt -o "$PROG" > "$TMP/err" 2>&1
        rc=$?
        if [ -n "$TIMEOUT" ] && [ "$rc" = 124 ]; then
            bad "$src $opt" "zaman asimi (60 sn): derleyici takildi"; continue
        elif [ "$rc" != 0 ] && grep -q 'kullanilamiyor: derleyici' "$TMP/err"; then
            # Derleyici bu modülün dış kütüphanesi (libcurl, libpq ...) olmadan derlenmiş.
            echo "  ATLANDI  $src $opt ($(head -1 "$TMP/err" | sed 's/;.*//'))"; skipped=$((skipped + 1)); continue
        elif [ "$rc" != 0 ]; then
            # (128'den büyük kod: derleyici bir sinyalle çöktü; sinyal = kod - 128)
            bad "$src $opt" "derlenemedi (cikis kodu $rc): $(head -3 "$TMP/err")"; continue
        fi
        : > "$TMP/replies"
        if [ -n "$port" ]; then
            MARLIN_STRING_CHECK=1 $TIMEOUT "$PROG" > "$TMP/actual" 2> "$TMP/stderr" &
            server=$!
            while IFS= read -r request; do
                $PY http_gonder.py "$port" "$request" >> "$TMP/replies" 2>&1
            done < <(sed -n 's|^// send: *||p' "$src")
            wait "$server"
            code=$?
        else
            MARLIN_STRING_CHECK=1 $TIMEOUT "$PROG" > "$TMP/actual" 2> "$TMP/stderr"
            code=$?
        fi
        if [ -n "$TIMEOUT" ] && [ "$code" = 124 ]; then
            bad "$src $opt" "zaman asimi (60 sn): program takildi"; continue
        fi
        if [ -n "$EXE" ]; then
            tr -d '\r' < "$TMP/actual" > "$TMP/actual.lf" && mv "$TMP/actual.lf" "$TMP/actual"
            tr -d '\r' < "$TMP/replies" > "$TMP/replies.lf" && mv "$TMP/replies.lf" "$TMP/replies"
        fi
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
        elif [ -n "$port" ] && ! diff -u "$TMP/expected_replies" "$TMP/replies" > "$TMP/diff"; then
            bad "$src $opt" "http yanitlari farkli:"; sed 's/^/           /' "$TMP/diff"
        else
            ok "$src $opt"
        fi
    done
done

# Dil sunucusu (editör eklentisinin kullandığı marlin_lang --lsp)
if $PY lsp_test.py "$COMPILER" > "$TMP/lsp" 2>&1; then
    ok "dil sunucusu (lsp_test.py)"
else
    bad "dil sunucusu (lsp_test.py)" "$(cat "$TMP/lsp")"
fi

echo "$pass gecti, $fail kaldi$( [ "$skipped" -gt 0 ] && echo ", $skipped atlandi")"
[ "$fail" -eq 0 ]
