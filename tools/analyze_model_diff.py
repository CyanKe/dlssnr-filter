#!/usr/bin/env python3
"""Is the difference between two DLSSNR networks a STYLE difference or a
CAPABILITY difference?

WHY THIS EXISTS
    "New vs old differ by mean|d| 1.6 grey levels" says nothing about which one is
    doing its job better. The distinction becomes measurable once the probe does
    not depend on taste. The key test is a decomposition:

        e_old = old - input          (what the old model did)
        e_new = new - input          (what the new model did)

      * cos(e_new, e_old) ~ 1 and a best-fit scale alpha < 1 means the two models
        perform THE SAME transformation, the new one just applying less of it.
        That is a magnitude/strength difference -- not a different recipe, and not
        a local failure.
      * The part scaling cannot explain (e_new - alpha*e_old) is where a genuine
        capacity gap would live. If THAT part concentrates on edges/texture and
        grows with content difficulty, the smaller network cannot reproduce the
        larger one's detail.
      * A different recipe (style) would instead show up as a low cosine, and as
        different global tone/colour shifts.

    So the numbers below separate three things that all look like "they differ":
    same transform at a different strength, a different transform, and detail the
    smaller network could not produce.

WHAT IT CANNOT DO
    It cannot say which output is "correct". For real footage nobody has the
    photoreal frame, so "correct" is undefined; what IS defined is whether the
    difference behaves like a capacity limit or like a re-tune.

Usage:
    python tools\\analyze_model_diff.py <input.png> <new.png> <old.png>
"""
import sys
from pathlib import Path

import numpy as np
from PIL import Image


def gray(path: Path) -> np.ndarray:
    a = np.asarray(Image.open(path).convert("RGB")).astype(np.float64)
    return 0.299 * a[:, :, 0] + 0.587 * a[:, :, 1] + 0.114 * a[:, :, 2]


def rgb(path: Path) -> np.ndarray:
    return np.asarray(Image.open(path).convert("RGB")).astype(np.float64)


def box_mean(x: np.ndarray, k: int) -> np.ndarray:
    """k x k box mean (reflect-padded) via an integral image: used to look at
    structure rather than at pixel noise."""
    pad = k // 2
    xp = np.pad(x, pad, mode="reflect")
    c = np.cumsum(np.cumsum(xp, axis=0), axis=1)
    c = np.pad(c, ((1, 0), (1, 0)))
    h, w = x.shape
    return (c[k:k + h, k:k + w] - c[0:h, k:k + w] - c[k:k + h, 0:w] + c[0:h, 0:w]) / (k * k)


def highpass(x: np.ndarray, k: int = 5) -> np.ndarray:
    return x - box_mean(x, k)


def grad_energy(x: np.ndarray) -> float:
    """Mean |dx| + |dy|: the detail proxy bench_procscale.cpp also uses."""
    return float(np.abs(np.diff(x, axis=1)).mean() + np.abs(np.diff(x, axis=0)).mean())


def lap_energy(x: np.ndarray) -> float:
    """Mean |4c - l - r - u - d|: curvature, i.e. how much fine structure is left."""
    c = x[1:-1, 1:-1]
    return float(np.abs(4 * c - x[1:-1, :-2] - x[1:-1, 2:] - x[:-2, 1:-1] - x[2:, 1:-1]).mean())


def main() -> int:
    if len(sys.argv) != 4:
        print(__doc__)
        return 1
    p_in, p_new, p_old = (Path(a) for a in sys.argv[1:4])

    src, new, old = gray(p_in), gray(p_new), gray(p_old)
    if src.shape != new.shape or src.shape != old.shape:
        print("inputs must have the same size")
        return 2

    e_new, e_old = new - src, old - src
    r = e_new - e_old                      # == new - old

    print("=== how much each model moved the picture ===")
    print(f"  old vs input : mean|d| {np.abs(e_old).mean():6.3f}   max {np.abs(e_old).max():.0f}")
    print(f"  new vs input : mean|d| {np.abs(e_new).mean():6.3f}   max {np.abs(e_new).max():.0f}")
    print(f"  new vs old   : mean|d| {np.abs(r).mean():6.3f}   max {np.abs(r).max():.0f}")

    # ---------------------------------------------------------------- key test
    # Best-fit scale of the old model's edit onto the new model's edit, and the
    # cosine between them. cos ~ 1 => same transformation.
    num = float((e_new * e_old).sum())
    den = float((e_old * e_old).sum())
    alpha = num / den if den else 0.0
    cos = num / (np.linalg.norm(e_new) * np.linalg.norm(e_old) + 1e-12)
    unexplained = e_new - alpha * e_old
    print("\n=== is it the SAME transformation at a different strength? ===")
    print(f"  cos(e_new, e_old)      = {cos:+.5f}   (1 = identical direction)")
    print(f"  best-fit scale alpha   = {alpha:.5f}   (new = alpha x old's edit)")
    print(f"  |new-old|              = {np.abs(r).mean():.3f} mean")
    print(f"  |new - alpha*old|      = {np.abs(unexplained).mean():.3f} mean  "
          f"-> scaling explains {100 * (1 - np.abs(unexplained).mean() / np.abs(r).mean()):.1f}% of it")

    # ------------------------------------------------------- global tone/colour
    c_new, c_old, c_src = rgb(p_new), rgb(p_old), rgb(p_in)
    print("\n=== global (tone/colour) -- a re-tune moves the whole picture ===")
    for i, ch in enumerate("RGB"):
        dn = (c_new[:, :, i] - c_src[:, :, i]).mean()
        do = (c_old[:, :, i] - c_src[:, :, i]).mean()
        print(f"  mean {ch} shift vs input : old {do:+7.3f}   new {dn:+7.3f}   "
              f"difference {dn - do:+.3f}")

    # ------------------------------------------- where does the gap live?
    g = np.hypot(*np.gradient(src))
    gb = box_mean(g, 9)
    corr_r = float(np.corrcoef(box_mean(np.abs(r), 9).ravel(), gb.ravel())[0, 1])
    corr_u = float(np.corrcoef(box_mean(np.abs(unexplained), 9).ravel(), gb.ravel())[0, 1])
    print("\n=== does the difference live where the picture is hard? ===")
    print(f"  corr(|new-old|, input detail)          = {corr_r:+.3f}")
    print(f"  corr(|unexplained|, input detail)      = {corr_u:+.3f}")
    print("     0 = the two differ equally everywhere; high = they differ where the")
    print("     content is detailed (the signature of a capacity limit)")

    hp_r = float(np.abs(highpass(r)).mean())
    hp_un = float(np.abs(highpass(unexplained)).mean())
    hp_eo = float(np.abs(highpass(e_old)).mean())
    hp_en = float(np.abs(highpass(e_new)).mean())
    # The absolute high-pass of the residual is smaller than the edits' simply
    # because the residual is a smaller signal. What matters is the high-frequency
    # FRACTION of each: a difference that is merely "less of the same effect"
    # inherits the edit's fraction, one that is about detail handling does not.
    fr_r, fr_un = hp_r / np.abs(r).mean(), hp_un / np.abs(unexplained).mean()
    fr_eo, fr_en = hp_eo / np.abs(e_old).mean(), hp_en / np.abs(e_new).mean()
    print("\n=== how much of each signal is fine detail (mean |x-blur| / mean |x|) ===")
    print(f"  old's edit {fr_eo:.4f}   new's edit {fr_en:.4f}   "
          f"residual {fr_r:.4f}   unexplained {fr_un:.4f}")
    print(f"  residual / old-edit = {fr_r / fr_eo:.2f}x")
    print("     ~1 = the difference is the same KIND of thing as the edit (a strength")
    print("     difference); >>1 = the difference sits disproportionately in fine detail")

    print("\n=== fine structure left in each output (higher = more detail) ===")
    print(f"  gradient energy : input {grad_energy(src):.4f}   new {grad_energy(new):.4f}   "
          f"old {grad_energy(old):.4f}   new/old {grad_energy(new) / grad_energy(old):.4f}")
    print(f"  laplacian energy: input {lap_energy(src):.4f}   new {lap_energy(new):.4f}   "
          f"old {lap_energy(old):.4f}   new/old {lap_energy(new) / lap_energy(old):.4f}")

    print("\n=== reading (evidence about the KIND of difference, not quality) ===")
    if cos > 0.99:
        print(f"  - The two models apply the SAME transformation (cos {cos:.4f}); the new one")
        print(f"    applies {100 * (1 - alpha):.1f}% less of it. That is a strength difference, not a")
        print("    different recipe.")
    elif cos > 0.95:
        print(f"  - Mostly the same transformation (cos {cos:.4f}) with a real residue: the")
        print("    remainder is where the two disagree about WHAT to do.")
    else:
        print(f"  - Low cosine ({cos:.4f}): these are materially different transformations,")
        print("    i.e. a recipe/style difference rather than a weaker copy.")
    if fr_eo > 0 and fr_r / fr_eo > 1.5:
        print(f"  - The residual is {fr_r / fr_eo:.1f}x richer in fine detail than the edits are,")
        print("    so a real part of the difference IS detail handling -- the smaller model")
        print("    resolves texture slightly differently. Check the absolute size below.")
    else:
        print(f"  - The residual is no richer in fine detail than the edits themselves")
        print(f"    (ratio {fr_r / fr_eo:.2f}), so it is not a detail-reconstruction failure.")
    dg = 100 * (grad_energy(new) / grad_energy(old) - 1)
    print(f"  - In absolute terms the fine structure differs by {dg:+.2f}% (gradient energy),")
    print("    which is the size of the whole detail question.")
    if corr_u < 0.25:
        print("  - The part scaling cannot explain does NOT grow with content difficulty,")
        print("    so it is not the 'fails on hard content' pattern.")
    else:
        print(f"  - The unexplained part tracks content difficulty ({corr_u:+.2f}), which is the")
        print("    pattern a capacity limit produces.")
    print("\n  None of this says which output is 'better': for real footage there is no")
    print("  reference frame. It says whether the difference behaves like a capacity limit")
    print("  or like a re-tune of the same operator.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
