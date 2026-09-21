#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
bench_engine.py - per-frame cost of the DLSSNR engine, by resolution.

WHAT THIS MEASURES
------------------
Exactly the quantity the filter's control panel reports as "ms/frame": the wall
time of ONE synchronous

    dlssnr2_process(inBgr, outBgr, reset)

call (BGR24 in / BGR24 out, w*h*3 bytes each).  src/dlssnr_dshow.cpp:628-645
wraps that same call in a QPC timer and feeds the result into the EMA the panel
displays, so a ctypes call timed here is the closest outside-the-player analogue
of the number users see.

WHAT THIS EXCLUDES
------------------
The filter's NV12 -> BGR24 and BGR24 -> NV12 conversion, DirectShow pin
plumbing, decoding and presentation.  All of those sit OUTSIDE the timed region
in the filter too, so the numbers stay directly comparable to the panel.

METHOD
------
* deterministic non-degenerate content: gradients + fine texture + a moving
  block + film grain, cycled over 4 distinct frames so the temporal denoiser is
  never fed a static image (a static image could hit a degenerate path);
* every resolution gets its own dlssnr2_init(), then a SUSTAINED pre-load
  (--warmup-sec, default 20 s). This matters more than it sounds: on this
  laptop the GPU sits at ~1.3 GHz for a short burst and only ramps to ~2.4 GHz
  after tens of seconds of continuous load, so a few-second burst silently
  reports the COLD cost - about 1.5x too high. Video playback is sustained, so
  the warmed-up number is the one that describes it;
* then the frame warmup, then R rounds of M frames, all with reset=0 (steady
  state - what playback actually sends);
* the first frame after a session start / seek carries reset=1, so that cost is
  measured separately;
* GPU clocks/power/temperature are sampled throughout: a per-frame time is only
  interpretable next to the clock state it was taken in, and the sampler is what
  catches a run that never left the low-clock state.

Usage:
    python tools\\bench_engine.py
    python tools\\bench_engine.py --quick
    python tools\\bench_engine.py --res 1280x720 3840x2160
"""

import argparse
import ctypes
import json
import statistics
import subprocess
import sys
import threading
import time
from pathlib import Path

import numpy as np

# ------------------------------------------------------------------ constants

# The resolutions the README documents. 1920x1440 is kept because the README
# already quotes a number for it.
DEFAULT_RES = [
    (1280, 720),
    (1920, 1080),
    (2560, 1440),
    (1920, 1440),
    (3840, 2160),
]

# Shipped defaults from app/dlssnr_dshow.ini.template (style=1 natural,
# intensity/localtone/localstruct = 100% = 1.0, skinstructure off, automask off).
DEFAULT_OPTS = dict(style=1, intensity=1.0, local_tone=1.0, local_struct=1.0,
                    skin_struct=0.0, auto_mask=0)

ROUNDS = 3
FRAMES_PER_ROUND = 40
WARMUP_FRAMES = 12
WARMUP_SEC = 20.0        # continuous pre-load per resolution; see sustain()


# -------------------------------------------------------------------- engine

class Engine:
    """Thin ctypes wrapper around dlssnr_host2.dll (the filter's own host)."""

    def __init__(self, host_dll: Path):
        if not host_dll.exists():
            raise SystemExit("host dll not found: %s" % host_dll)
        self.appdir = host_dll.parent
        self.lib = ctypes.CDLL(str(host_dll))
        lib = self.lib
        lib.dlssnr2_set_appdir.argtypes = [ctypes.c_wchar_p]
        lib.dlssnr2_set_appdir.restype = None
        lib.dlssnr2_init.argtypes = [ctypes.c_int, ctypes.c_int, ctypes.c_wchar_p]
        lib.dlssnr2_init.restype = ctypes.c_int
        lib.dlssnr2_process.argtypes = [ctypes.c_void_p, ctypes.c_void_p, ctypes.c_int]
        lib.dlssnr2_process.restype = ctypes.c_int
        lib.dlssnr2_set_options.argtypes = [ctypes.c_int, ctypes.c_float, ctypes.c_float,
                                            ctypes.c_float, ctypes.c_float, ctypes.c_int,
                                            ctypes.c_int]
        lib.dlssnr2_set_options.restype = None
        lib.dlssnr2_get_sizes.argtypes = [ctypes.POINTER(ctypes.c_int),
                                          ctypes.POINTER(ctypes.c_int)]
        lib.dlssnr2_get_sizes.restype = None
        # Point the host at its own folder so it finds nvngx_dlssnr.dll.
        lib.dlssnr2_set_appdir(str(self.appdir))

    def init(self, w, h, log_path: Path):
        t0 = time.perf_counter()
        rc = self.lib.dlssnr2_init(w, h, str(log_path))
        return rc, (time.perf_counter() - t0) * 1000.0

    def set_options(self, o):
        self.lib.dlssnr2_set_options(int(o["style"]), float(o["intensity"]),
                                     float(o["local_tone"]), float(o["local_struct"]),
                                     float(o["skin_struct"]), int(o["auto_mask"]), 0)

    def session_size(self):
        w = ctypes.c_int(0)
        h = ctypes.c_int(0)
        self.lib.dlssnr2_get_sizes(ctypes.byref(w), ctypes.byref(h))
        return w.value, h.value


# ------------------------------------------------------------------- content

def synth_frames(w, h, count=4, seed=1234):
    """Deterministic, non-trivial BGR24 frames.

    Content is deliberately *not* static: the engine is a temporal denoiser, and
    a frozen image is not what playback sends. A moving block plus per-frame
    grain keeps the temporal work realistic.
    """
    rng = np.random.default_rng(seed)
    yy, xx = np.mgrid[0:h, 0:w].astype(np.float32)
    u = xx / max(w - 1, 1)
    v = yy / max(h - 1, 1)

    base = np.zeros((h, w, 3), np.float32)
    base[..., 0] = 30.0 + 90.0 * u + 25.0 * v                    # B
    base[..., 1] = 35.0 + 70.0 * v + 20.0 * (1.0 - u)            # G
    base[..., 2] = 45.0 + 80.0 * (1.0 - u) + 30.0 * v            # R
    # Fine texture: gives the network real high-frequency detail to reconstruct.
    tex = (np.sin(xx * 0.9) * np.sin(yy * 0.7) + np.sin((xx + yy) * 0.31))
    base += (tex * 8.0)[..., None]

    frames = []
    for k in range(count):
        f = base.copy()
        bh, bw = max(h // 6, 8), max(w // 6, 8)
        y0 = int((h - bh) * (k / float(count)))
        x0 = int((w - bw) * (0.37 * k % 1.0))
        f[y0:y0 + bh, x0:x0 + bw] += 60.0
        f += rng.normal(0.0, 3.0, f.shape)
        frames.append(np.ascontiguousarray(np.clip(f, 0.0, 255.0).astype(np.uint8)))
    return frames


def load_raw_frames(path, w, h, count=4):
    """Load the first `count` packed BGR24 frames from a raw file (see --raw).

    Lets the harness run on REAL 4K frames instead of synthetic ones - useful
    because the panel's number has to hold on real content, and because odd
    geometries (e.g. 3840x2076, the letterboxed 4K height on some BluRays) are
    worth testing explicitly.
    """
    need = w * h * 3
    data = np.fromfile(str(path), dtype=np.uint8)
    have = len(data) // need
    if have < 1:
        raise SystemExit("raw file too small for %dx%d (need %d bytes/frame)" % (w, h, need))
    n = min(count, have)
    return [np.ascontiguousarray(data[i * need:(i + 1) * need].reshape(h, w, 3))
            for i in range(n)]


# ---------------------------------------------------------------- gpu sample

class GpuSampler(threading.Thread):
    """Samples nvidia-smi in the background so the numbers carry their context."""

    QUERY = ("clocks.sm,clocks.max.sm,clocks.mem,power.draw,power.limit,"
             "temperature.gpu,utilization.gpu")

    def __init__(self, period=0.25):
        super().__init__(daemon=True)
        self.period = period
        self.samples = []           # (t, sm, smmax, power, plimit, temp, util)
        self._stop_evt = threading.Event()

    def run(self):
        while not self._stop_evt.is_set():
            try:
                out = subprocess.run(
                    ["nvidia-smi", "--query-gpu=" + self.QUERY,
                     "--format=csv,noheader,nounits"],
                    capture_output=True, text=True, timeout=5)
                if out.returncode == 0:
                    parts = [p.strip() for p in out.stdout.strip().split(",")]
                    vals = [float(p) if p not in ("", "[N/A]", "N/A") else None
                            for p in parts]
                    self.samples.append((time.perf_counter(),) + tuple(vals))
            except Exception:
                pass
            self._stop_evt.wait(self.period)

    def stop(self):
        self._stop_evt.set()


def summarise(samples, t0, t1):
    """Aggregate GPU samples that fall inside [t0, t1]."""
    win = [s for s in samples if t0 <= s[0] <= t1]
    if not win:
        return {}
    def col(i):
        vals = [s[i] for s in win if s[i] is not None]
        return vals
    out = {"n": len(win)}
    for name, i in (("sm_clock", 1), ("sm_clock_max", 2), ("mem_clock", 3),
                    ("power_w", 4), ("power_limit_w", 5), ("temp_c", 6),
                    ("util_pct", 7)):
        vals = col(i)
        if vals:
            out[name + "_avg"] = round(statistics.fmean(vals), 1)
            out[name + "_min"] = round(min(vals), 1)
            out[name + "_max"] = round(max(vals), 1)
    return out


# --------------------------------------------------------------------- sweep

def sm_clock_now():
    """Current SM clock in MHz, or None if nvidia-smi is unavailable."""
    try:
        out = subprocess.run(["nvidia-smi", "--query-gpu=clocks.sm",
                              "--format=csv,noheader,nounits"],
                             capture_output=True, text=True, timeout=5)
        if out.returncode == 0:
            return float(out.stdout.strip().splitlines()[0])
    except Exception:
        pass
    return None


def sustain(engine, inptrs, outptr, seconds, pace_fps=None):
    """Keep the engine busy until the GPU has ramped to its steady clock.

    Measured on this machine: the SAME resolution costs ~1.5x more in a 3 s
    burst than it does after ~20 s of continuous load, because the platform
    holds ~1.3 GHz until it decides the load is real and then goes to ~2.4 GHz.
    Sampling without this phase therefore reports the cold cost, not the cost of
    watching a video.
    """
    n = 0
    period = (1.0 / pace_fps) if pace_fps else 0.0
    t_next = time.perf_counter()
    deadline = t_next + seconds
    while time.perf_counter() < deadline:
        engine.lib.dlssnr2_process(inptrs[n % len(inptrs)], outptr, 0)
        n += 1
        if period:
            t_next += period
            slack = t_next - time.perf_counter()
            if slack > 0:
                time.sleep(slack)
            else:
                t_next = time.perf_counter()
    return n


def measure(engine, w, h, frames, opts, rounds, per_round, warmup, reset_first=True,
            pace_fps=None):
    """Time dlssnr2_process() only. Returns (list_of_ms, first_frame_ms)."""
    out = np.zeros((h, w, 4), np.uint8)          # oversized on purpose: >= w*h*3
    outptr = out.ctypes.data_as(ctypes.c_void_p)
    inbufs = frames
    inptrs = [f.ctypes.data_as(ctypes.c_void_p) for f in inbufs]

    # Guard against the one mistake this harness can make: the host trusts the
    # caller's buffer geometry, so passing e.g. 720p buffers to a 2160p session
    # is an access violation, not an error return.
    sw, sh = engine.session_size()
    if (sw, sh) != (w, h):
        raise RuntimeError("session is %dx%d but %dx%d buffers were passed - "
                           "call dlssnr2_init() first" % (sw, sh, w, h))

    engine.set_options(opts)

    first_ms = None
    if reset_first:
        t0 = time.perf_counter()
        rc = engine.lib.dlssnr2_process(inptrs[0], outptr, 1)
        first_ms = (time.perf_counter() - t0) * 1000.0
        if rc == 0:
            raise RuntimeError("dlssnr2_process(reset=1) failed at %dx%d" % (w, h))

    # pace_fps simulates the player: the streaming thread is not free-running,
    # it is admitted once per frame period by the renderer. That leaves the GPU
    # idle - the question is whether it also drops out of the high clock state.
    period = (1.0 / pace_fps) if pace_fps else 0.0
    t_next = time.perf_counter()

    def pace():
        nonlocal t_next
        if not period:
            return
        t_next += period
        slack = t_next - time.perf_counter()
        if slack > 0:
            time.sleep(slack)
        else:
            t_next = time.perf_counter()

    samples = []
    for _ in range(rounds):
        for i in range(warmup):
            engine.lib.dlssnr2_process(inptrs[i % len(inptrs)], outptr, 0)
            pace()
        for i in range(per_round):
            p = inptrs[(i + 1) % len(inptrs)]
            t0 = time.perf_counter()
            rc = engine.lib.dlssnr2_process(p, outptr, 0)
            dt = (time.perf_counter() - t0) * 1000.0
            if rc == 0:
                raise RuntimeError("dlssnr2_process failed at %dx%d" % (w, h))
            samples.append(dt)
            pace()
    return samples, first_ms


def stats(ms):
    ms = sorted(ms)
    n = len(ms)
    def pct(p):
        if n == 1:
            return ms[0]
        idx = max(0, min(n - 1, int(round((p / 100.0) * (n - 1)))))
        return ms[idx]
    return {
        "n": n,
        "median": round(statistics.median(ms), 2),
        "mean": round(statistics.fmean(ms), 2),
        "min": round(ms[0], 2),
        "p10": round(pct(10), 2),
        "p90": round(pct(90), 2),
        "max": round(ms[-1], 2),
        "stdev": round(statistics.pstdev(ms), 2),
    }


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--res", nargs="+", default=None,
                    help="resolutions as WxH, e.g. 1280x720 3840x2160")
    ap.add_argument("--quick", action="store_true", help="3 rounds x 10 frames")
    ap.add_argument("--rounds", type=int, default=None)
    ap.add_argument("--frames", type=int, default=None)
    ap.add_argument("--warmup-sec", type=float, default=None,
                    help="seconds of continuous pre-load before sampling so the GPU "
                         "reaches its steady clock (default 20, --quick 3)")
    ap.add_argument("--automask", action="store_true",
                    help="also measure with the automatic mask enabled")
    ap.add_argument("--raw", default=None,
                    help="use real packed BGR24 frames from this file instead of "
                         "synthetic content (top-down, no padding)")
    ap.add_argument("--raw-size", default=None, help="WxH of the --raw frames")
    ap.add_argument("--pace-fps", type=float, default=None,
                    help="simulate real playback pacing (e.g. 60): sleep so frames "
                         "arrive at this rate instead of running the engine back to back")
    ap.add_argument("--out", default=None, help="JSON output path")
    args = ap.parse_args()

    root = Path(__file__).resolve().parent.parent
    host = root / "app" / "dlssnr_host2.dll"
    log = root / "app" / "dlssnr_bench.log"
    out_json = Path(args.out) if args.out else (root / "tools" / "bench_engine_results.json")

    res = DEFAULT_RES
    if args.res:
        res = []
        for s in args.res:
            w, h = s.lower().split("x")
            res.append((int(w), int(h)))

    rounds = args.rounds if args.rounds else (3 if args.quick else ROUNDS)
    per_round = args.frames if args.frames else (10 if args.quick else FRAMES_PER_ROUND)
    warmup_sec = args.warmup_sec if args.warmup_sec is not None else (3.0 if args.quick else WARMUP_SEC)

    print("host   : %s" % host)
    print("rounds : %d x %d frames (+%d warmup each), reset=0" % (rounds, per_round, WARMUP_FRAMES))
    print("preload: %.0f s of load per resolution before sampling" % warmup_sec)
    if args.pace_fps:
        print("pace   : simulating %.0f fps playback (period %.1f ms)"
              % (args.pace_fps, 1000.0 / args.pace_fps))
    print()

    raw_frames = None
    raw_wh = None
    if args.raw:
        if not args.raw_size:
            raise SystemExit("--raw requires --raw-size WxH")
        rw, rh = (int(v) for v in args.raw_size.lower().split("x"))
        raw_frames = load_raw_frames(args.raw, rw, rh)
        raw_wh = (rw, rh)
        print("content: %d real frames from %s (%dx%d)" % (len(raw_frames), args.raw, rw, rh))

    engine = Engine(host)
    sampler = GpuSampler()
    sampler.start()

    results = {
        "host_dll": str(host),
        "rounds": rounds,
        "frames_per_round": per_round,
        "warmup_frames": WARMUP_FRAMES,
        "options": DEFAULT_OPTS,
        "resolutions": [],
    }

    for (w, h) in res:
        entry = {"w": w, "h": h}
        print("=== %dx%d (%.2f MPix) ===" % (w, h, w * h / 1e6))
        t_a = time.perf_counter()
        rc, init_ms = engine.init(w, h, log)
        entry["init_ms"] = round(init_ms, 1)
        if rc != 1:
            entry["error"] = "dlssnr2_init failed"
            print("    init FAILED - skipping")
            results["resolutions"].append(entry)
            continue
        sw, sh = engine.session_size()
        entry["session_w"], entry["session_h"] = sw, sh
        print("    init %.1f ms, session %dx%d" % (init_ms, sw, sh))

        if raw_frames is not None and raw_wh == (w, h):
            frames = raw_frames
            print("    content: real frames")
        else:
            frames = synth_frames(w, h)

        # Pre-load until the platform stops holding the low clock. Skipping this
        # is the single easiest way to publish numbers ~1.5x too slow.
        hold_out = np.zeros((h, w, 4), np.uint8)
        hold_in = [f.ctypes.data_as(ctypes.c_void_p) for f in frames]
        clk_before = sm_clock_now()
        n_pre = sustain(engine, hold_in, hold_out.ctypes.data_as(ctypes.c_void_p),
                        warmup_sec, args.pace_fps)
        clk_after = sm_clock_now()
        entry.update(warmup_sec=warmup_sec, warmup_frames=n_pre,
                     clock_before_mhz=clk_before, clock_after_mhz=clk_after)
        print("    pre-load %.0f s / %d frames: SM clock %s -> %s MHz"
              % (warmup_sec, n_pre, clk_before, clk_after))

        try:
            ms, first_ms = measure(engine, w, h, frames, DEFAULT_OPTS,
                                   rounds, per_round, WARMUP_FRAMES,
                                   pace_fps=args.pace_fps)
        except RuntimeError as e:
            entry["error"] = str(e)
            print("    ERROR %s" % e)
            results["resolutions"].append(entry)
            continue

        entry["steady"] = stats(ms)
        entry["first_frame_ms"] = None if first_ms is None else round(first_ms, 2)
        entry["raw_ms"] = [round(x, 3) for x in ms]
        print("    steady state: median %.2f ms (mean %.2f, p10 %.2f, p90 %.2f, min %.2f)"
              % (entry["steady"]["median"], entry["steady"]["mean"],
                 entry["steady"]["p10"], entry["steady"]["p90"], entry["steady"]["min"]))
        print("    first frame (reset=1): %.2f ms" % (first_ms or 0.0))

        if args.automask:
            o = dict(DEFAULT_OPTS)
            o["auto_mask"] = 1
            ms_am, _ = measure(engine, w, h, frames, o, rounds, per_round,
                               WARMUP_FRAMES, reset_first=False)
            entry["automask"] = stats(ms_am)
            print("    automask=1   : median %.2f ms (delta %+.2f)"
                  % (entry["automask"]["median"],
                     entry["automask"]["median"] - entry["steady"]["median"]))

        t_b = time.perf_counter()
        entry["gpu"] = summarise(sampler.samples, t_a, t_b)
        results["resolutions"].append(entry)
        print()

    # ---- drift check: repeat the first resolution at the end. The power wall
    # documented in the toolkit report means late-in-run numbers can sag, and a
    # README table that silently depends on run order would be useless.
    if len(res) > 1 and not args.quick:
        w, h = res[0]
        print("=== drift check: %dx%d again, at the end of the run ===" % (w, h))
        rc_drift, _ = engine.init(w, h, log)   # session is still at the last size
        frames = synth_frames(w, h)
        hold_out = np.zeros((h, w, 4), np.uint8)
        sustain(engine, [f.ctypes.data_as(ctypes.c_void_p) for f in frames],
                hold_out.ctypes.data_as(ctypes.c_void_p), warmup_sec, args.pace_fps)
        try:
            ms, _ = measure(engine, w, h, frames, DEFAULT_OPTS, rounds, per_round,
                            WARMUP_FRAMES, reset_first=False, pace_fps=args.pace_fps)
            s = stats(ms)
            first = next((e for e in results["resolutions"]
                          if e["w"] == w and e["h"] == h and "steady" in e), None)
            results["drift_check"] = {
                "w": w, "h": h, "steady": s,
                "first_pass_median": first["steady"]["median"] if first else None,
            }
            print("    median %.2f ms (first pass %.2f ms)"
                  % (s["median"], first["steady"]["median"] if first else float("nan")))
            print()
        except RuntimeError as e:
            print("    drift check failed: %s" % e)

    sampler.stop()
    sampler.join(timeout=2)

    results["gpu_overall"] = summarise(sampler.samples, 0.0, float("inf"))
    out_json.parent.mkdir(parents=True, exist_ok=True)
    out_json.write_text(json.dumps(results, indent=2), encoding="utf-8")
    print("raw timings -> %s" % out_json)

    # ---- markdown table for the README
    print()
    print("| resolution | MPix | median ms | mean ms | p10-p90 ms | fps @median | ns/pixel |")
    print("| --- | --- | --- | --- | --- | --- | --- |")
    for e in results["resolutions"]:
        if "steady" not in e:
            continue
        s = e["steady"]
        mp = e["w"] * e["h"] / 1e6
        fps = 1000.0 / s["median"]
        nsp = s["median"] * 1e6 / (e["w"] * e["h"])
        print("| %dx%d | %.2f | %.2f | %.2f | %.2f-%.2f | %.0f | %.2f |"
              % (e["w"], e["h"], mp, s["median"], s["mean"], s["p10"], s["p90"], fps, nsp))

    g = results.get("gpu_overall", {})
    if g:
        print()
        print("GPU during run: SM clock %.0f-%.0f MHz (avg %.0f), power avg %.1f W "
              "(limit %.0f), temp avg %.1f C, util avg %.0f%%"
              % (g.get("sm_clock_min", 0), g.get("sm_clock_max", 0),
                 g.get("sm_clock_avg", 0), g.get("power_w_avg", 0),
                 g.get("power_limit_w_avg", 0), g.get("temp_c_avg", 0),
                 g.get("util_pct_avg", 0)))


if __name__ == "__main__":
    sys.exit(main())
