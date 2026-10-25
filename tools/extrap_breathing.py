#!/usr/bin/env python3
"""Motion-robust breathing metric for real footage.

The synthetic clip's metric (level std of one sequence) assumes a static flat
region. Real footage does not have one: the flattest 128x128 window in this clip
still varies by 6 grey levels over time, which swamps anything the network does.

So measure the DIFFERENCE instead. `e_k - L_k` is extrapolation minus the real
layer for the same frame; whatever the content does -- grain, a camera pan, a
lighting change -- is common to both and cancels exactly. What survives is the
error's own drift over time, which is the "breathing" we actually care about:
a constant bias is harmless, a wandering one is visible.

Reported per target layer count:
  std_t[ mean(e_k - L_k) ]      low-frequency wander of the extrapolation error
  std_t[ mean|e_k - L_k| ]      the same for its magnitude
  bias                          the steady part (harmless if it is large and stable)

How to produce the inputs (all four scripts share this layout):

    tools\\build_frame_dump.bat
    build\\frame_dump.exe in.yuv d_in.yuv  1920 1080 56 --layers 1 --appdir app
    build\\frame_dump.exe in.yuv d_L1.yuv  1920 1080 56 --layers 1 --appdir app
    build\\frame_dump.exe in.yuv d_L2.yuv  1920 1080 56 --layers 2 --appdir app
    ...                                                       --layers 3, 4

Note --skip 8 (the default) FEEDS 8 frames without writing them, so dump frame j
is input frame j + 8. That is why the scripts take a skip argument.

    python tools\\extrap_breathing.py <dir> <prefix> [W] [H] [skip]
"""
import os
import sys
import numpy as np

WIN = 128


def main():
    d = sys.argv[1] if len(sys.argv) > 1 else "."
    pre = sys.argv[2] if len(sys.argv) > 2 else "real"
    W = int(sys.argv[3]) if len(sys.argv) > 3 else 1920
    H = int(sys.argv[4]) if len(sys.argv) > 4 else 1080
    OFF = int(sys.argv[5]) if len(sys.argv) > 5 else 8
    YSZ, FSZ = W * H, W * H * 3 // 2

    fin = open(f"{d}/{pre}_in.yuv", "rb")
    outs = {k: open(f"{d}/{pre}_L{k}.yuv", "rb") for k in (1, 2, 3, 4)}
    n = os.path.getsize(f"{d}/{pre}_L1.yuv") // FSZ

    def rd(f, t):
        f.seek(t * FSZ)
        return np.frombuffer(f.read(YSZ), np.uint8).reshape(H, W).astype(np.float32)

    # Window: spatially flattest area of the median frame. Spatial flatness is
    # all we need -- the differential cancels whatever the content does next.
    mid = rd(fin, n // 2 + OFF)
    gy, gx = np.gradient(mid)
    g = np.abs(gx) + np.abs(gy)
    best, by, bx = 1e30, 0, 0
    for y in range(0, H - WIN + 1, 16):
        for x in range(0, W - WIN + 1, 16):
            s = g[y:y + WIN, x:x + WIN].mean()
            if s < best:
                best, by, bx = s, y, x
    Y0, Y1, X0, X1 = by, by + WIN, bx, bx + WIN
    print(f"spatially flattest {WIN}x{WIN} at ({bx},{by})  "
          f"gradient {best:.3f}  level {mid[Y0:Y1, X0:X1].mean():.1f}  "
          f"({n} frames)")

    gains = {2: (2.0, 1.80), 3: (3.0, 2.40), 4: (4.0, 2.85)}
    series = {k: {g: [] for g in gains[k]} for k in (2, 3, 4)}
    absr = {k: {g: [] for g in gains[k]} for k in (2, 3, 4)}
    glob = {k: {g: [] for g in gains[k]} for k in (2, 3, 4)}

    for t in range(n):
        a = rd(fin, t + OFF)
        o1 = rd(outs[1], t)
        v1 = o1 - a
        for k, gs in gains.items():
            ok = rd(outs[k], t)
            for gg in gs:
                e = np.clip(a + gg * v1, 0.0, 255.0)
                dif = e - ok
                series[k][gg].append(float(dif[Y0:Y1, X0:X1].mean()))
                absr[k][gg].append(float(np.abs(dif[Y0:Y1, X0:X1]).mean()))
                glob[k][gg].append(float(np.abs(dif).mean()))

    print()
    print("=" * 84)
    print("EXTRAPOLATION ERROR OVER TIME (8-bit levels)")
    print("=" * 84)
    print(f"{'N':>2} {'gain':>5} {'bias':>7} {'std_t(bias)':>12} {'mean|d|':>8} "
          f"{'std_t(|d|)':>11} {'global mean|d|':>15}")
    for k in (2, 3, 4):
        for gg, tag in ((gains[k][0], "k=N"), (gains[k][1], "best")):
            s = np.array(series[k][gg])
            ab = np.array(absr[k][gg])
            print(f"{k:>2} {tag:>5} {s.mean():>7.3f} {s.std():>12.3f} "
                  f"{ab.mean():>8.3f} {ab.std():>11.3f} "
                  f"{np.mean(glob[k][gg]):>15.3f}")

    print()
    print("Reading: 'bias' is the steady part of the error (a fixed offset -- not")
    print("visible as flicker as long as it is stable). 'std_t' is what moves, and")
    print("a moving error on a stationary shot IS the breathing artefact.")

    for f in outs.values():
        f.close()
    fin.close()


if __name__ == "__main__":
    main()
