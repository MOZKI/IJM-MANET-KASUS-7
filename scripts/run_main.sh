#!/usr/bin/env bash
# CP4: eksperimen utama, A dan B untuk seed 1-14 dan 16 (30 run). Jalankan dari folder ns-3-dev:
#   export REPO=/path/ke/IJM-MANET-KASUS-7
#   caffeinate -i nohup bash $REPO/scripts/run_main.sh > $REPO/results/run_main.log 2>&1 &
# Aman dihentikan lalu dijalankan ulang: run yang CSV-nya sudah ada dilewati.
# Paksa ulang semua: FORCE=1 bash ...
set -uo pipefail
REPO="${REPO:?isi REPO=/path/ke/IJM-MANET-KASUS-7}"
OUT="${OUT:-$REPO/results/raw}"
RANGE="${RANGE:-250}"
# Seed 15 dikeluarkan: simulasi macet (loop ICMP/AODV di ns-3, CPU 100% tanpa selesai) di A dan B,
# jadi diganti seed 16. Seed 1-14 dan 16 = 15 seed. Catat sebagai anomali di laporan.
SEEDS="${SEEDS:-$(seq 1 14) 16}"
TIMEOUT="${TIMEOUT:-120}"   # detik per run; run normal cuma 1-2 detik
mkdir -p "$OUT"

# Catatan reproduksibilitas (parameter + versi), ditulis sekali di awal
INFO="$OUT/RUN_INFO.txt"
{
  echo "tanggal        : $(date '+%Y-%m-%d %H:%M:%S %z')"
  echo "ns-3 VERSION   : $(cat VERSION 2>/dev/null || echo unknown)"
  echo "ns-3 git       : $(git describe --tags --always 2>/dev/null || echo unknown)"
  echo "repo commit    : $(git -C "$REPO" rev-parse --short HEAD 2>/dev/null || echo 'belum di-commit')"
  echo "sha256 skrip   : $(shasum -a 256 "$REPO/sim/manet-kasus7.cc" | cut -d' ' -f1)"
  echo "perintah       : ./ns3 run \"manet-kasus7 --variant={A,B} --seed={1..15} --range=$RANGE --outDir=$OUT\""
  echo "parameter lain : default di skrip (nMobile=12, nStatic=4, simTime=600, warmup=60)"
  echo "seed           : $(echo $SEEDS | tr '\n' ' ')"
  echo "catatan         : seed 15 macet (A dan B), diganti seed 16"
  echo "-----"
} >> "$INFO"

./ns3 build || { echo "build gagal"; exit 1; }

n=0; total=$(( $(echo $SEEDS | wc -w) * 2 )); fail=0
for s in $SEEDS; do
  for v in A B; do
    n=$((n+1))
    seedStr=$(printf "%02d" "$s")
    f="$OUT/result_${v}_seed${seedStr}.csv"
    if [[ -f "$f" && -z "${FORCE:-}" ]]; then
      echo "[$n/$total] lewati $v seed=$s (CSV sudah ada)"; continue
    fi
    echo "[$n/$total] $(date +%H:%M:%S) variant=$v seed=$s"
    if ! perl -e 'alarm shift; exec @ARGV' "$TIMEOUT" ./ns3 run --no-build "manet-kasus7 --variant=$v --seed=$s --range=$RANGE --outDir=$OUT"; then
      echo "!!! GAGAL/TIMEOUT variant=$v seed=$s"; fail=$((fail+1))
      pkill -f ns3.49-manet-kasus7 || true
    fi
  done
done
echo "Selesai $(date +%H:%M:%S). Gagal: $fail. Verifikasi: python3 $REPO/analysis/verify_raw.py $OUT"
