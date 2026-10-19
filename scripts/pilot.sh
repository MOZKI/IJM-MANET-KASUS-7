#!/usr/bin/env bash
# Pilot CP3. Jalankan dari folder ns-3-dev:
#   REPO=/path/ke/IJM-MANET-KASUS-7 bash $REPO/scripts/pilot.sh
# Ganti radius / seed lewat env: RANGE=300 SEEDS="101 102 103" ...
# Tiap radius masuk folder sendiri supaya hasil 250 m vs 300 m tidak saling menimpa.
set -euo pipefail
REPO="${REPO:?isi REPO=/path/ke/IJM-MANET-KASUS-7}"
RANGE="${RANGE:-250}"
SEEDS="${SEEDS:-101 102 103}"
OUT="${OUT:-$REPO/results/pilot_r${RANGE}}"
mkdir -p "$OUT"
for s in $SEEDS; do
  for v in A B; do
    echo ">>> variant=$v seed=$s range=$RANGE"
    ./ns3 run "manet-kasus7 --variant=$v --seed=$s --range=$RANGE --outDir=$OUT"
  done
done
echo "Selesai. Cek: python3 $REPO/analysis/check_pilot.py $OUT"
