"""Verifikasi hasil CP4 sebelum analisis. Pakai: python3 analysis/verify_raw.py results/raw"""
import glob
import sys

import pandas as pd

d = sys.argv[1]
files = sorted(glob.glob(f"{d}/result_*.csv"))
df = pd.concat(pd.read_csv(f) for f in files)
bad = 0
def check(ok, msg):
    global bad
    print(("LOLOS" if ok else "GAGAL"), "|", msg)
    bad += (not ok)

need = {"hello_bytes", "fwd_static_data", "conn_pair_pct"}
check(need <= set(df.columns), "semua CSV berformat kolom baru (ada hello/fwd/conn)")
if not need <= set(df.columns):
    sys.exit("ada CSV lama (tanpa kolom baru). Hapus lalu jalankan ulang run itu dengan FORCE=1")

# Seed 15 macet di simulator dan diganti seed 16 (lihat RUN_INFO.txt dan laporan)
seeds = set(range(1, 15)) | {16}
for v in "AB":
    got = set(df[df.variant == v].seed)
    check(got == seeds, f"varian {v}: seed lengkap 1-14 dan 16 (kurang: {sorted(seeds - got) or '-'})")
check(len(df) == 30 and not df.duplicated(["variant", "seed"]).any(), f"30 baris unik (ada {len(df)})")
for c in ["nMobile", "range_m", "simTime_s", "warmup_s"]:
    check(df[c].nunique() == 1, f"{c} sama di semua run ({sorted(df[c].unique())})")
check(set(df[df.variant == "A"].nStatic) == {0} and set(df[df.variant == "B"].nStatic) == {4}, "nStatic: A=0, B=4")
s = df.pivot(index="seed", columns="variant", values="sent")
check((s.A == s.B).all(), "paket terkirim sama di A dan B per seed (trafik identik)")
cp = df.pivot(index="seed", columns="variant", values="conn_pair_pct")
check((cp.B >= cp.A - 1e-9).all(), "konektivitas B >= A per seed (gerak identik, pos hanya menambah)")
check(df.isna().sum().sum() == 0 and df.pdr_pct.between(0, 100).all(), "tidak ada NaN, PDR dalam 0-100")
check((df[df.variant == "B"].fwd_static_data > 0).all(), "node statis meneruskan data di semua run B")
print("\nSEMUA AMAN" if bad == 0 else f"\n{bad} cek gagal, periksa dulu sebelum CP5")
print(f"\nPDR rata-rata kasar: A {df[df.variant=='A'].pdr_pct.mean():.1f}% | B {df[df.variant=='B'].pdr_pct.mean():.1f}%")
