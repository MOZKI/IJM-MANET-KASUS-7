"""Ringkas hasil pilot CP3 dan cek kriteria validasi.
Pakai: python3 analysis/check_pilot.py results/pilot_r250
Ambang di bawah adalah USULAN (ASUMSI), tetapkan sebelum melihat hasil dan catat di laporan."""
import glob
import sys

import pandas as pd

A_MIN_PDR = 20.0      # PDR A di bawah ini = "mendekati 0%"
B_CEILING_PDR = 98.0  # PDR B >= ini di SEMUA seed = langit-langit, beda tidak terlihat
A_MIN_CONN = 50.0     # konektivitas pasangan A di bawah ini = indikasi partisi berat

d = sys.argv[1]
files = sorted(glob.glob(f"{d}/result_*.csv"))
if not files:
    sys.exit(f"tidak ada CSV di {d}")
df = pd.concat(pd.read_csv(f) for f in files).sort_values(["seed", "variant"])
df["ovh_nohello"] = (df.ctrl_bytes - df.hello_bytes) / df.rcvd_bytes
df["hello_share_pct"] = 100 * df.hello_bytes / df.ctrl_bytes
tot = df.fwd_static_data + df.fwd_mobile_data
df["static_relay_share_pct"] = (100 * df.fwd_static_data / tot).where(tot > 0, 0.0)

cols = ["variant", "seed", "range_m", "sent", "rcvd", "pdr_pct", "mean_delay_s", "overhead",
        "ovh_nohello", "hello_share_pct", "conn_pair_pct", "conn_all_pct",
        "fwd_static_data", "fwd_mobile_data", "static_relay_share_pct"]
pd.set_option("display.width", 250)
pd.set_option("display.max_columns", 30)
print(df[cols].round(3).to_string(index=False))

A, B = df[df.variant == "A"], df[df.variant == "B"]
print("\n=== CEK CP3 ===")
ok = lambda c: "LOLOS" if c else "GAGAL"
print(ok((A.pdr_pct >= A_MIN_PDR).all()), f"| PDR A tidak mendekati 0% (min {A.pdr_pct.min():.1f}%, ambang {A_MIN_PDR}%)")
print(ok((A.conn_pair_pct >= A_MIN_CONN).all()), f"| tidak terpartisi total di A (konektivitas pasangan min {A.conn_pair_pct.min():.1f}%)")
print(ok(not (B.pdr_pct >= B_CEILING_PDR).all()), f"| PDR B tidak ~100% di semua seed (rentang {B.pdr_pct.min():.1f}-{B.pdr_pct.max():.1f}%)")
print(ok((B.fwd_static_data > 0).all()), f"| node statis meneruskan data di B (min {int(B.fwd_static_data.min())} paket/run)")
print(ok((A.fwd_static_data == 0).all()), "| sanity: tidak ada forward 'statis' di A")
print(ok((A.fwd_mobile_data > 0).all()), "| sanity: multi-hop lewat petugas terjadi di A")
print("\nInfo (bukan lolos/gagal):")
for v, g in (("A", A), ("B", B)):
    print(f"  {v}: PDR rata-rata {g.pdr_pct.mean():.1f}% | konektivitas pasangan {g.conn_pair_pct.mean():.1f}% | jaringan utuh {g.conn_all_pct.mean():.1f}% waktu")
print(f"  selisih PDR B-A rata-rata: {B.pdr_pct.mean() - A.pdr_pct.mean():.1f} poin")
print(f"  hello = {df.hello_share_pct.mean():.0f}% byte kontrol; overhead tanpa hello A {A.ovh_nohello.mean():.2f} vs B {B.ovh_nohello.mean():.2f}")
