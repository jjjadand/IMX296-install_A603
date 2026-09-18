#!/usr/bin/env python3
"""Rebuild isp/imx296-isp.json from the Raspberry Pi / libcamera tuning JSON."""
import json
from pathlib import Path

root = Path(__file__).resolve().parent
tuning = json.loads((root / "imx296_16mm.json").read_text())

def algo(name):
    for a in tuning["algorithms"]:
        if name in a:
            return a[name]
    raise KeyError(name)

black = algo("rpi.black_level")["black_level"]
awb = algo("rpi.awb")
ccm_tbl = algo("rpi.ccm")["ccms"]
gamma = algo("rpi.contrast")["gamma_curve"]
ct = awb["ct_curve"]
cts, rs, bs = [], [], []
for i in range(0, len(ct), 3):
    cts.append(ct[i]); rs.append(ct[i + 1]); bs.append(ct[i + 2])

def q10(x):
    return int(round(x * 1024))

best = min(ccm_tbl, key=lambda c: abs(c["ct"] - 4560))
gx, gy = gamma[0::2], gamma[1::2]
gamma_lut, k, n = [], 0, len(gx)
for i in range(1024):
    x = i * 64
    while k + 1 < n and gx[k + 1] < x:
        k += 1
    if k + 1 >= n or x <= gx[k]:
        y = gy[k]
    else:
        x0, x1 = gx[k], gx[k + 1]
        y0, y1 = gy[k], gy[k + 1]
        y = y0 + (y1 - y0) * (x - x0) / (x1 - x0)
    gamma_lut.append(int((y * 255 + 32767) / 65535))

cfg = {
    "source": "isp/imx296_16mm.json (Raspberry Pi / libcamera IMX296 16mm)",
    "cfa": "rggb",
    "black_10": int(round(black / 64)),
    "ccm_ct": best["ct"],
    "ccm_q10": [q10(v) for v in best["ccm"]],
    "ct_kelvin": cts,
    "ct_r_q10": [q10(r) for r in rs],
    "ct_b_q10": [q10(b) for b in bs],
    "ct_gain_r_q10": [q10(1.0 / r) for r in rs],
    "ct_gain_b_q10": [q10(1.0 / b) for b in bs],
    "default_ct_index": 2,
    "hl_start": 780,
    "hl_full": 940,
    "fc_start": 220,
    "fc_full": 500,
    "fc_ratio_lo": 250,
    "fc_ratio_hi": 650,
    "gamma_lut": gamma_lut,
}
(root / "imx296-isp.json").write_text(json.dumps(cfg, indent=2) + "
")
print("wrote", root / "imx296-isp.json")
