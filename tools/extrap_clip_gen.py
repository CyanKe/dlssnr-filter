#!/usr/bin/env python3
"""Synthetic NV12 test clip for the layer-extrapolation A/B.

Content is chosen for what we have to measure, not for looking nice:

  * a LARGE flat patch  -> the only place NR really works, and the place the
    temporal-stability metric (file: ENGINEERING_NOTES §9, 0.196 levels) lives;
  * hard vertical edges + a fine checkerboard -> local tone / structure have
    something to bite on, and overshoot from extrapolation would ring here;
  * per-pixel temporal noise, uncorrelated between frames -> the network has a
    reason to move, and a second pass has residuals to find;
  * constant chroma -> the analysis is about luma, chroma would only add noise.

Deterministic: one fixed seed, no clock, no filesystem ordering.

    python tools\\extrap_clip_gen.py [out.yuv] [frames]

This is the SYNTHETIC source: it exists because a real clip has no region flat
enough for the level-stability metric (see extrap_breathing.py for the fix).

Written to limited range (16..235) because that is what the engine's BT.709
integer path assumes.
"""
import sys
import numpy as np

W, H = 1920, 1080
N = 48
SEED = 20261025

# Flat patch, in pixels. Big enough to survive a 256x256 analysis window.
FX0, FY0, FX1, FY1 = 1400, 100, 1800, 340


def frame(t, rng):
    yy, xx = np.mgrid[0:H, 0:W].astype(np.float32)

    # --- smooth base: a gradient that drifts slowly, so nothing is static ---
    drift = np.sin(t * 0.13) * 18.0
    base = 40.0 + 130.0 * (xx / (W - 1)) + drift

    # --- hard edge: a moving vertical bar (two strong discontinuities) ---
    bar_x = 120.0 + (W - 400.0) * (t / max(N - 1, 1))
    base += 55.0 * ((xx > bar_x) & (xx < bar_x + 120.0))

    # --- fine detail: a 8px checkerboard in a band, exercises "structure" ---
    band = (yy > 700) & (yy < 860) & (xx > 200) & (xx < 1200)
    checker = (((xx // 8).astype(np.int32) + (yy // 8).astype(np.int32)) & 1).astype(np.float32)
    base = base + np.where(band, (checker - 0.5) * 70.0, 0.0)

    # --- the flat patch: constant level, only noise moves inside it ---
    patch = (yy >= FY0) & (yy < FY1) & (xx >= FX0) & (xx < FX1)
    base = np.where(patch, 128.0, base)

    # --- temporal noise everywhere (uncorrelated frame to frame) ---
    base = base + rng.normal(0.0, 4.0, size=(H, W)).astype(np.float32)

    y = np.clip(base, 16.0, 235.0)

    uv = np.full((H // 2, W // 2, 2), 128, dtype=np.uint8)
    return y.astype(np.uint8), uv


def main():
    out = sys.argv[1] if len(sys.argv) > 1 else r"build\extrap_in.yuv"
    n = int(sys.argv[2]) if len(sys.argv) > 2 else N
    rng = np.random.default_rng(SEED)
    with open(out, "wb") as f:
        for t in range(n):
            y, uv = frame(t, rng)
            f.write(y.tobytes())
            f.write(uv.tobytes())
    print(f"wrote {out}: {W}x{H}, {n} frames, {W * H * 3 // 2 * n / 1048576:.1f} MiB")


if __name__ == "__main__":
    main()
