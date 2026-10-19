#!/usr/bin/env bash
# GPU ölçüm düzeneği: bench/*.marl programlarını -O2 ile derler, her birini
# CUDA ve CPU'da (istenirse Vulkan'da) TEKRAR kez çalıştırır, en iyi süreyi
# yazar. Sağlama satırları arka uçlar arasında aynı olmalıdır.
#
# Kullanım: bench/olc.sh [marlin] [arka uçlar, varsayılan "cuda cpu"]
#   TEKRAR=5 bench/olc.sh
#   bench/olc.sh ../cmake-build-debug/marlin "cuda vulkan cpu"
set -u
cd "$(dirname "$0")"
COMPILER=${1:-../cmake-build-debug/marlin}
BACKENDS=${2:-cuda cpu}
TEKRAR=${TEKRAR:-3}
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

printf '%-10s' program
for b in $BACKENDS; do printf '%14s' "$b"; done
printf '   sagla\n'
for src in *.marl; do
    name=${src%.marl}
    "$COMPILER" "$src" -O2 -o "$TMP/$name" > "$TMP/err" 2>&1 || { echo "$name: derlenemedi"; cat "$TMP/err"; continue; }
    printf '%-10s' "$name"
    checks=""
    for b in $BACKENDS; do
        best=""
        for _ in $(seq 1 "$TEKRAR"); do
            MARLIN_GPU=$b MARLIN_GPU_REPORT=1 timeout 300 "$TMP/$name" > "$TMP/out" 2> "$TMP/stderr"
            v=$(awk 'NR==1 {print $2}' "$TMP/out")
            if [ -z "$best" ] || awk -v a="$v" -v b="$best" 'BEGIN{exit !(a<b)}'; then best=$v; fi
        done
        used=$(grep -o '^marlin gpu: [a-z]*' "$TMP/stderr" | head -1 | awk '{print $3}')
        printf '%14s' "$(printf '%.2f' "$best")${used:+/$used}"
        checks="$checks | $(sed -n 2p "$TMP/out")"
    done
    printf '  %s\n' "$checks"
done
echo "(birim: programın ilk satırı — _ms milisaniye, _us mikrosaniye; /arka uç: gerçekten kullanılan)"
