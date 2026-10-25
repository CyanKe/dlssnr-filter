#!/usr/bin/env python3
"""Why does extrapolated k=1.80 not match real 2 layers in COLOUR?

The earlier analysis measured the Y plane only, so it could not see this. Two
candidate causes, and they call for different fixes:

  (a) chroma overshoot -- amplifying the UV delta by 1.8 pushes 8-bit chroma
      past its legal range, and clipping chroma is what turns into visible
      magenta/cyan banding;
  (b) the network's colour response is simply not colinear with luma's, so
      scaling UV by the luma-derived gain is the wrong gain for chroma.

Measured here, on the 48-frame segment:

  * UV error of extrapolation vs real L2, against the baseline L1 vs L2
  * how many UV bytes land on 0 or 255 (clipping) in each
  * the same with chroma LEFT ALONE (L1's UV): does the colour error vanish?
"""
import os
import sys

import numpy as np

W, H = 1920, 1080
YSZ, UVSZ = W * H, W * H // 2
FSZ = YSZ + UVSZ
OFF, N = 8, 48
K = 1.80


def rd(f, t):
    f.seek(t * FSZ)
    y = np.frombuffer(f.read(YSZ), np.uint8).reshape(H, W).astype(np.float32)
    uv = np.frombuffer(f.read(UVSZ), np.uint8).reshape(H // 2, W).astype(np.float32)
    return y, uv


PFX = sys.argv[1] if len(sys.argv) > 1 else "real"
D = sys.argv[2] if len(sys.argv) > 2 else "."
fin = open(os.path.join(D, PFX + "_in.yuv"), "rb")
f1 = open(os.path.join(D, PFX + "_L1.yuv"), "rb")
f2 = open(os.path.join(D, PFX + "_L2.yuv"), "rb")

acc = dict(err_uv=0.0, ref_uv=0.0, err_y=0.0, ref_y=0.0,
           clip_e=0, clip_1=0, clip_2=0, err_uv_lo=0.0)
for t in range(N):
    y0, u0 = rd(fin, t + OFF)
    y1, u1 = rd(f1, t)
    y2, u2 = rd(f2, t)

    ey = np.clip(np.rint(y0 + K * (y1 - y0)), 0, 255)
    eu_raw = np.rint(u0 + K * (u1 - u0))
    eu = np.clip(eu_raw, 0, 255)

    acc["err_y"] += float(np.abs(ey - y2).mean())
    acc["ref_y"] += float(np.abs(y1 - y2).mean())
    acc["err_uv"] += float(np.abs(eu - u2).mean())
    acc["ref_uv"] += float(np.abs(u1 - u2).mean())
    # luma-only variant: leave chroma at layer 1
    acc["err_uv_lo"] += float(np.abs(u1 - u2).mean())
    acc["clip_e"] += int(((eu_raw < 0) | (eu_raw > 255)).sum())
    acc["clip_1"] += int(((u1 == 0) | (u1 == 255)).sum())
    acc["clip_2"] += int(((u2 == 0) | (u2 == 255)).sum())

n = float(N)
uv_px = float(UVSZ * N)
print(f"{'plane':>6} {'extrap k=1.80 err':>19} {'baseline L1-L2':>16} {'reproduced':>11}")
print(f"{'Y':>6} {acc['err_y']/n:>19.3f} {acc['ref_y']/n:>16.3f} "
      f"{100.0*(1-acc['err_y']/acc['ref_y']):>10.1f}%")
print(f"{'UV':>6} {acc['err_uv']/n:>19.3f} {acc['ref_uv']/n:>16.3f} "
      f"{100.0*(1-acc['err_uv']/acc['ref_uv']):>10.1f}%")

print()
print(f"UV bytes clipped to 0/255, per UV sample:")
print(f"  extrapolated k=1.80 : {100.0*acc['clip_e']/uv_px:.3f}%")
print(f"  real L1             : {100.0*acc['clip_1']/uv_px:.3f}%")
print(f"  real L2             : {100.0*acc['clip_2']/uv_px:.3f}%")

print()
print("If chroma is left at L1 (luma-only extrapolation):")
print(f"  UV err vs real L2 = {acc['err_uv_lo']/n:.3f} "
      f"(this is just L1's own chroma error, unavoidable)")
print(f"  vs {acc['err_uv']/n:.3f} from extrapolating chroma too")

fin.close(); f1.close(); f2.close()
