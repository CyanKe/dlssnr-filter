#!/usr/bin/env python3
"""Real-clip A/B: real 1..4 layers vs extrapolation of the 1-layer correction.

Same questions as analyze.py/analyze2.py, but resolution-agnostic and with the
flat region FOUND rather than hard-coded -- a real clip has no rectangle we can
point at in advance, and picking one by hand is how you accidentally measure
whatever flatters the conclusion.

The flat window is chosen from the INPUT by scoring every 128x128 window on
  temporal variance (a static region) + 8 * spatial gradient (a uniform region)
and taking the minimum. Both are what "flat" has to mean for the breathing
metric to say anything: a region that is uniform but drifts is not flat, and a
region that is static but textured hides the level wander in its own detail.

How to produce the inputs (all four scripts share this layout):

    tools\\build_frame_dump.bat
    build\\frame_dump.exe in.yuv d_in.yuv  1920 1080 56 --layers 1 --appdir app
    build\\frame_dump.exe in.yuv d_L1.yuv  1920 1080 56 --layers 1 --appdir app
    build\\frame_dump.exe in.yuv d_L2.yuv  1920 1080 56 --layers 2 --appdir app
    ...                                                       --layers 3, 4

Note --skip 8 (the default) FEEDS 8 frames without writing them, so dump frame j
is input frame j + 8. That is why the scripts take a skip argument.

    python tools\\extrap_layers.py <dir> <prefix> [W] [H] [skip]
"""
import sys
import numpy as np

WIN = 128
STRIDE = 32


def asm(px):
    return np.frombuffer(px, dtype=np.uint8).reshape(H, W).astype(np.float32)


def read_y(f, t):
    f.seek(t * FSZ)
    return asm(f.read(YSZ))


def main():
    d = sys.argv[1] if len(sys.argv) > 1 else "."
    pre = sys.argv[2] if len(sys.argv) > 2 else "real"
    global W, H, YSZ, FSZ
    W = int(sys.argv[3]) if len(sys.argv) > 3 else 1920
    H = int(sys.argv[4]) if len(sys.argv) > 4 else 1080
    OFF = int(sys.argv[5]) if len(sys.argv) > 5 else 8
    YSZ, FSZ = W * H, W * H * 3 // 2

    fin = open(f"{d}/{pre}_in.yuv", "rb")
    outs = {k: open(f"{d}/{pre}_L{k}.yuv", "rb") for k in (1, 2, 3, 4)}
    n = (len(open(f"{d}/{pre}_L1.yuv", "rb").read(1)) and 0) or None
    import os
    n = os.path.getsize(f"{d}/{pre}_L1.yuv") // FSZ
    print(f"{W}x{H}, {n} dumped frames (input offset +{OFF})")

    # ---------- pass A: find the flattest window in the INPUT ----------
    s1 = np.zeros((H, W), np.float64)
    s2 = np.zeros((H, W), np.float64)
    for t in range(n):
        f = read_y(fin, t + OFF).astype(np.float64)
        s1 += f
        s2 += f * f
    mean = s1 / n
    var = np.maximum(s2 / n - mean * mean, 0.0)
    gy, gx = np.gradient(mean)
    grad = np.abs(gx) + np.abs(gy)
    best, by, bx = 1e30, 0, 0
    for y in range(0, H - WIN + 1, STRIDE):
        for x in range(0, W - WIN + 1, STRIDE):
            score = var[y:y + WIN, x:x + WIN].mean() + 8.0 * grad[y:y + WIN, x:x + WIN].mean()
            if score < best:
                best, by, bx = score, y, x
    print(f"flattest {WIN}x{WIN} window at ({bx},{by})  "
          f"temporal sd {np.sqrt(var[by:by+WIN, bx:bx+WIN].mean()):.3f}  "
          f"mean level {mean[by:by+WIN, bx:bx+WIN].mean():.1f}")
    Y0, Y1, X0, X1 = by, by + WIN, bx, bx + WIN

    # ---------- pass B: everything ----------
    KS = np.arange(0.50, 4.01, 0.05)
    ks_best = {2: 1.80, 3: 2.40, 4: 2.90}     # from the synthetic run; re-derived below
    err_n = {k: 0.0 for k in (1, 2, 3, 4)}
    p2_n = {k: 0 for k in (1, 2, 3, 4)}
    mx_n = {k: 0.0 for k in (1, 2, 3, 4)}
    dref = {k: 0.0 for k in (1, 2, 3, 4)}
    sweep = {t: np.zeros(len(KS)) for t in (2, 3, 4)}
    sw_n = 0
    clip_lo = {k: 0 for k in (1, 2, 3, 4)}
    clip_hi = {k: 0 for k in (1, 2, 3, 4)}
    dot = {k: 0.0 for k in (2, 3, 4)}
    n1sq = 0.0
    nksq = {k: 0.0 for k in (2, 3, 4)}
    tf = {}
    lvl = {}
    for name in ["in"] + [f"L{k}" for k in (1, 2, 3, 4)] + [f"e{k}" for k in (1, 2, 3, 4)]:
        tf[name] = 0.0
        lvl[name] = []
    prev = None
    sub = (slice(None, None, 4), slice(None, None, 4))     # sweep on 1/16 of pixels

    for t in range(n):
        a = read_y(fin, t + OFF)
        o = {k: read_y(outs[k], t) for k in (1, 2, 3, 4)}
        v1 = o[1] - a
        emu = {}
        for k in (1, 2, 3, 4):
            raw = a + k * v1
            clip_lo[k] += int((raw < 16.0).sum())
            clip_hi[k] += int((raw > 235.0).sum())
            e = np.clip(raw, 0.0, 255.0)
            emu[k] = e
            dd = np.abs(e - o[k])
            err_n[k] += float(dd.sum())
            mx_n[k] = max(mx_n[k], float(dd.max()))
            p2_n[k] += int((dd > 2.0).sum())
            if k > 1:
                dref[k] += float(np.abs(o[1] - o[k]).sum())
        for tk in (2, 3, 4):
            tgt = (o[tk] - a)[sub]
            for i, k in enumerate(KS):
                sweep[tk][i] += float(np.abs(k * v1[sub] - tgt).mean())
        sw_n += 1

        n1sq += float((v1 * v1).sum())
        for k in (2, 3, 4):
            vk = o[k] - o[k - 1]
            dot[k] += float((v1 * vk).sum())
            nksq[k] += float((vk * vk).sum())

        seqs = {"in": a, **{f"L{k}": o[k] for k in (1, 2, 3, 4)},
                **{f"e{k}": emu[k] for k in (1, 2, 3, 4)}}
        for name, img in seqs.items():
            lvl[name].append(float(img[Y0:Y1, X0:X1].mean()))
        if prev is not None:
            for name, img in seqs.items():
                tf[name] += float(np.abs(img - prev[name]).mean())
        prev = seqs

    px = float(YSZ * n)
    print()
    print("=" * 86)
    print("1) DISTANCE -- extrapolation vs the real layer  (mean |d| in 8-bit levels)")
    print("=" * 86)
    print(f"{'N':>2} {'k=N':>6} {'err@k=N':>8} {'%px>2':>7} {'max':>4} | "
          f"{'best k':>7} {'err@best':>9} | {'|out1-outN|':>12} {'explained':>10}")
    for k in (1, 2, 3, 4):
        a_err = err_n[k] / px
        if k == 1:
            print(f"{k:>2} {1:>6} {a_err:>8.3f} {100.0*p2_n[k]/px:>6.2f}% "
                  f"{mx_n[k]:>4.0f} | {'-':>7} {'-':>9} | {'-':>12} {'exact':>10}")
            continue
        e = sweep[k] / sw_n
        i = int(np.argmin(e))
        kb = float(KS[i])
        ref = dref[k] / px
        # exact error at the best gain (sweep ran on a subsample)
        ex = 0.0
        for t in range(n):
            aa = read_y(fin, t + OFF)
            oo = read_y(outs[k], t)
            ex += float(np.abs(np.clip(aa + kb * (read_y(outs[1], t) - aa), 0, 255) - oo).mean())
        ex /= n
        print(f"{k:>2} {k:>6} {a_err:>8.3f} {100.0*p2_n[k]/px:>6.2f}% {mx_n[k]:>4.0f} | "
              f"{kb:>7.2f} {ex:>9.3f} | {ref:>12.3f} {100.0*(1-ex/ref):>9.1f}%")
        ks_best[k] = kb

    print()
    print("=" * 86)
    print("2) DIRECTION -- collinear? and does each layer's step shrink?")
    print("=" * 86)
    n1 = np.sqrt(n1sq)
    print(f"{'k':>2} {'|vk|/|v1|':>10} {'cos(v1,vk)':>11}   geometric gain   opt gain")
    geo = 1.0
    for k in (2, 3, 4):
        nk = np.sqrt(nksq[k])
        ratio = nk / n1
        cos = dot[k] / (n1 * nk)
        geo += ratio
        print(f"{k:>2} {ratio:>10.3f} {cos:>11.3f}   {geo:>14.2f} {ks_best[k]:>10.2f}")

    print()
    print("=" * 86)
    print("3) TEMPORAL -- flat-window level breathing (8-bit levels)")
    print("=" * 86)
    print(f"{'seq':>5} {'mean|dF| global':>16} {'mean|dF| window':>16} {'level std':>10}")
    for name in ("in", "L1", "L2", "L3", "L4", "e1", "e2", "e3", "e4"):
        print(f"{name:>5} {tf[name]/(n-1):>16.4f} {'-':>16} {np.std(lvl[name]):>10.4f}")

    print()
    print("=" * 86)
    print("4) CLIPPING -- share of the extrapolated frame leaving limited range")
    print("=" * 86)
    for k in (1, 2, 3, 4):
        print(f"  k={k}: below 16 {100.0*clip_lo[k]/px:6.3f}%   "
              f"above 235 {100.0*clip_hi[k]/px:6.3f}%")

    for f in outs.values():
        f.close()
    fin.close()


if __name__ == "__main__":
    main()
