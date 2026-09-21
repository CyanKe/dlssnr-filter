#!/usr/bin/env python3
# bench_verify.py -- INDEPENDENT verifier for dlssnr_host2.dll (DLSS Neural Render,
# Feature 18), written from the published binary contract only:
#     docs/ENGINE_INTERFACE.md  +  src/dlssnr_host2.cpp (exports / buffer comments).
# It shares NO code with the Lead's tools/bench_engine.py.
#
# Contract:
#   dlssnr2_init(w,h,logPath) -> 1 ok; re-init at a new size in-process is supported
#   dlssnr2_process(inBgr, outBgr, reset)        sync, BGR24 in / BGR24 out (w*h*3 each)
#   dlssnr2_process_rgba(inBgr, outRgba, reset)  sync, BGR24 in / RGBA8 out
#   dlssnr2_submit(inBgr, reset)  -> 1 ok, non-blocking, at most 2 frames in flight
#   dlssnr2_fetch(outRgba, outBgr) -> frames left in flight, -1 err; waits for OLDEST
#   dlssnr2_pending() / dlssnr2_drain()
#
# Nothing GPU-touching runs at import or before the first dlssnr2_init().

import argparse
import ctypes
import json
import os
import subprocess
import sys
import threading
import time

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
APP_DIR = r"D:\VSCODE\dlssnr-filter\app"
HOST_DLL = os.path.join(APP_DIR, "dlssnr_host2.dll")
HOST_LOG = os.path.join(APP_DIR, "bench_verify_host.log")
OUT_JSON = os.path.join(HERE, "bench_verify_results.json")
OUT_MD = os.path.join(HERE, "bench_verify_report.md")

CREATE_NO_WINDOW = 0x08000000
SENT = 0xA5
DISTINCT_FRAMES = 4          # cycle distinct moving frames like real playback
DEFAULT_STYLE = 1            # app/dlssnr_dshow.ini.template default
ALL_RES = [("1280x720", 1280, 720), ("1920x1080", 1920, 1080),
           ("2560x1440", 2560, 1440), ("3840x2160", 3840, 2160)]
CONTENTS = ["flat", "gradient", "noise", "detail"]
MAIN_CONTENT = "noise"       # worst-case detail is the primary scaling content
STYLE_PROBE_RES = ((1920, 1080), (3840, 2160))


def load_lib():
    if not os.path.exists(HOST_DLL):
        raise FileNotFoundError(HOST_DLL)
    try:
        os.add_dll_directory(APP_DIR)
    except Exception:
        pass
    lib = ctypes.CDLL(HOST_DLL)
    lib.dlssnr2_set_appdir.argtypes = [ctypes.c_wchar_p]
    lib.dlssnr2_set_appdir.restype = None
    lib.dlssnr2_init.argtypes = [ctypes.c_int, ctypes.c_int, ctypes.c_wchar_p]
    lib.dlssnr2_init.restype = ctypes.c_int
    lib.dlssnr2_process.argtypes = [ctypes.c_void_p, ctypes.c_void_p, ctypes.c_int]
    lib.dlssnr2_process.restype = ctypes.c_int
    lib.dlssnr2_process_rgba.argtypes = [ctypes.c_void_p, ctypes.c_void_p, ctypes.c_int]
    lib.dlssnr2_process_rgba.restype = ctypes.c_int
    lib.dlssnr2_submit.argtypes = [ctypes.c_void_p, ctypes.c_int]
    lib.dlssnr2_submit.restype = ctypes.c_int
    lib.dlssnr2_fetch.argtypes = [ctypes.c_void_p, ctypes.c_void_p]
    lib.dlssnr2_fetch.restype = ctypes.c_int
    lib.dlssnr2_pending.argtypes = []
    lib.dlssnr2_pending.restype = ctypes.c_int
    lib.dlssnr2_drain.argtypes = []
    lib.dlssnr2_drain.restype = ctypes.c_int
    lib.dlssnr2_set_options.argtypes = [ctypes.c_int, ctypes.c_float, ctypes.c_float,
                                        ctypes.c_float, ctypes.c_float, ctypes.c_int,
                                        ctypes.c_int]
    lib.dlssnr2_set_options.restype = None
    lib.dlssnr2_get_sizes.argtypes = [ctypes.POINTER(ctypes.c_int),
                                      ctypes.POINTER(ctypes.c_int)]
    lib.dlssnr2_get_sizes.restype = None
    lib.dlssnr2_shutdown.argtypes = []
    lib.dlssnr2_shutdown.restype = None
    return lib


def ptr(a):
    return a.ctypes.data_as(ctypes.c_void_p)


def set_options(lib, style=DEFAULT_STYLE, intensity=1.0, local_tone=1.0,
                local_struct=1.0, skin=0.0, auto_mask=0, ui_correction=0):
    # Values mirror app/dlssnr_dshow.ini.template (the filter's shipped defaults).
    lib.dlssnr2_set_options(int(style), float(intensity), float(local_tone),
                            float(local_struct), float(skin), int(auto_mask),
                            int(ui_correction))


# ------------------------------------------------------------------- content
def make_frames(name, w, h, count=DISTINCT_FRAMES):
    """Return `count` distinct BGR24 frames. Real playback never sends a frozen
    image, and this engine is a temporal denoiser, so the primary content moves."""
    rng = np.random.default_rng(1234)
    frames = []
    if name == "flat":
        a = np.empty((h, w, 3), np.uint8)
        a[:] = 96
        return [a.copy() for _ in range(count)]

    xs = np.arange(w, dtype=np.float32)[None, :]
    ys = np.arange(h, dtype=np.float32)[:, None]

    if name == "gradient":
        base = np.empty((h, w, 3), np.float32)
        base[..., 0] = xs * 255.0 / max(1, w - 1)
        base[..., 1] = ys * 255.0 / max(1, h - 1)
        base[..., 2] = 128.0
    elif name == "detail":
        xsi = np.arange(w, dtype=np.int32)[None, :]
        ysi = np.arange(h, dtype=np.int32)[:, None]
        chk = (((xsi // 6) + (ysi // 6)) & 1).astype(np.float32) * 200.0 + 28.0
        r2 = (xsi - w * 0.5).astype(np.float32) ** 2 + (ysi - h * 0.5).astype(np.float32) ** 2
        rings = np.sin(np.sqrt(r2) / 3.0) * 90.0 + 128.0
        base = np.empty((h, w, 3), np.float32)
        base[..., 0] = chk
        base[..., 1] = rings
        base[..., 2] = 0.5 * (chk + rings)
    elif name == "noise":
        base = None
    else:
        raise ValueError(name)

    bh, bw = max(h // 6, 8), max(w // 6, 8)
    for k in range(count):
        if name == "noise":
            f = np.random.default_rng(1000 + k).integers(0, 256, (h, w, 3), dtype=np.uint8
                                                         ).astype(np.float32)
        else:
            f = base.copy()
            # moving bright block: content changes every frame
            y0 = int((h - bh) * (k / float(count)))
            x0 = int((w - bw) * ((0.37 * k) % 1.0))
            f[y0:y0 + bh, x0:x0 + bw] += 60.0
        f += rng.normal(0.0, 3.0, (h, w, 3))
        frames.append(np.ascontiguousarray(np.clip(f, 0.0, 255.0).astype(np.uint8)))
    return frames


# ------------------------------------------------------------------ GPU state
def sample_gpu_once():
    q = ("timestamp,temperature.gpu,power.draw,power.limit,clocks.sm,"
         "clocks.mem,utilization.gpu,utilization.memory")
    try:
        r = subprocess.run(["nvidia-smi", "--query-gpu=" + q,
                            "--format=csv,noheader,nounits"],
                           capture_output=True, text=True, timeout=5,
                           creationflags=CREATE_NO_WINDOW)
    except Exception:
        return None
    if r.returncode != 0 or not r.stdout.strip():
        return None
    parts = [x.strip() for x in r.stdout.strip().splitlines()[0].split(",")]
    if len(parts) < 8:
        return None

    def f(x):
        try:
            return float(x)
        except Exception:
            return None

    return {"wall": time.time(), "timestamp": parts[0], "temp_c": f(parts[1]),
            "power_w": f(parts[2]), "power_limit_w": f(parts[3]), "sm_mhz": f(parts[4]),
            "mem_mhz": f(parts[5]), "util_gpu_pct": f(parts[6]),
            "util_mem_pct": f(parts[7])}


class GpuSampler(threading.Thread):
    def __init__(self, interval=0.2):
        super().__init__(daemon=True)
        self.interval = interval
        self._stop_evt = threading.Event()
        self.samples = []

    def run(self):
        while not self._stop_evt.is_set():
            s = sample_gpu_once()
            if s:
                self.samples.append(s)
            self._stop_evt.wait(self.interval)

    def stop(self):
        self._stop_evt.set()
        try:
            self.join(timeout=3.0)
        except Exception:
            pass

    def report(self, t0=None, t1=None):
        if t0 is None:
            win = list(self.samples)
        else:
            win = [s for s in self.samples if t0 <= s["wall"] <= (t1 if t1 else 1e18)]
        if not win:
            return {"note": "no nvidia-smi samples in window"}
        keys = ["power_w", "power_limit_w", "temp_c", "sm_mhz", "mem_mhz",
                "util_gpu_pct", "util_mem_pct"]
        out = {"n_samples": len(win)}
        for k in keys:
            vals = [s[k] for s in win if s.get(k) is not None]
            if vals:
                out[k] = {"min": round(min(vals), 1),
                          "mean": round(sum(vals) / len(vals), 1),
                          "max": round(max(vals), 1)}
        return out


# ------------------------------------------------------------------ statistics
def stats(ms):
    a = np.asarray(ms, dtype=np.float64)
    if a.size == 0:
        return {}
    p10, p50, p90 = np.percentile(a, [10, 50, 90])
    return {"n": int(a.size), "median": round(float(p50), 3),
            "mean": round(float(a.mean()), 3), "p10": round(float(p10), 3),
            "p90": round(float(p90), 3), "min": round(float(a.min()), 3),
            "max": round(float(a.max()), 3),
            "stdev": round(float(a.std(ddof=1)) if a.size > 1 else 0.0, 3)}


def merge(rounds):
    out = []
    for r in rounds:
        out.extend(r)
    return out


# ------------------------------------------------------------------ engine ops
def engine_init(lib, w, h):
    t0 = time.perf_counter()
    ok = lib.dlssnr2_init(w, h, HOST_LOG)
    dt_ms = (time.perf_counter() - t0) * 1000.0
    if ok != 1:
        raise RuntimeError("dlssnr2_init failed at %dx%d (see %s)" % (w, h, HOST_LOG))
    set_options(lib)                      # filter's shipped defaults
    gw, gh = ctypes.c_int(0), ctypes.c_int(0)
    lib.dlssnr2_get_sizes(ctypes.byref(gw), ctypes.byref(gh))
    return round(dt_ms, 2), (gw.value, gh.value)


def warmup(lib, frames, out3, n):
    nf = len(frames)
    for i in range(n):
        if lib.dlssnr2_process(ptr(frames[i % nf]), ptr(out3),
                               1 if i == 0 else 0) != 1:
            raise RuntimeError("warmup dlssnr2_process failed at frame %d" % i)


def run_sync_rounds(lib, frames, out3, warmup_n, rounds, count):
    per_round = []
    nf = len(frames)
    idx = 0
    for _ in range(rounds):
        warmup(lib, frames, out3, warmup_n)
        ms = []
        for _ in range(count):
            f = frames[idx % nf]
            idx += 1
            t0 = time.perf_counter()
            ok = lib.dlssnr2_process(ptr(f), ptr(out3), 0)
            t1 = time.perf_counter()
            if ok != 1:
                raise RuntimeError("dlssnr2_process failed")
            ms.append((t1 - t0) * 1000.0)
        per_round.append(ms)
    return per_round


def run_pipeline(lib, frames, out_bgr, count):
    """Two slots: prime both, then FETCH-THEN-SUBMIT. submit-then-fetch would
    drain every step (and submit at pending==2 returns 0 / 'pipeline full'),
    i.e. it would re-measure the sync path."""
    nf = len(frames)
    if lib.dlssnr2_submit(ptr(frames[0]), 1) != 1:
        raise RuntimeError("pipeline prime #1 failed")
    if lib.dlssnr2_submit(ptr(frames[1 % nf]), 0) != 1:
        raise RuntimeError("pipeline prime #2 failed")
    submit_ms, fetch_ms, iter_ms = [], [], []
    for k in range(count):
        t0 = time.perf_counter()
        left = lib.dlssnr2_fetch(None, ptr(out_bgr))
        t1 = time.perf_counter()
        if left < 0:
            raise RuntimeError("dlssnr2_fetch failed at k=%d" % k)
        ok = lib.dlssnr2_submit(ptr(frames[(2 + k) % nf]), 0)
        t2 = time.perf_counter()
        if ok != 1:
            raise RuntimeError("dlssnr2_submit failed at k=%d pending=%d"
                               % (k, lib.dlssnr2_pending()))
        fetch_ms.append((t1 - t0) * 1000.0)
        submit_ms.append((t2 - t1) * 1000.0)
        iter_ms.append((t2 - t0) * 1000.0)
    lib.dlssnr2_drain()
    if lib.dlssnr2_pending() != 0:
        raise RuntimeError("drain left pending=%d" % lib.dlssnr2_pending())
    return submit_ms, fetch_ms, iter_ms


# ---------------------------------------------------------------- size probe
def size_probe(lib, w, h, mode, in3):
    """Sentinel probe: fill an oversized output buffer, call the entry point, and
    report exactly which byte ranges were written. mode in {'bgr','rgba'}."""
    plane3, plane4 = w * h * 3, w * h * 4
    guard = 1 << 20                                  # 1 MiB tail guard
    n = plane4 + guard
    buf = np.full(n, SENT, np.uint8)
    if mode == "bgr":
        ok = lib.dlssnr2_process(ptr(in3), ptr(buf), 0)
    else:
        ok = lib.dlssnr2_process_rgba(ptr(in3), ptr(buf), 0)
    if ok != 1:
        raise RuntimeError("size probe (%s) failed" % mode)
    diff = (buf != np.uint8(SENT))

    def frac(a, b):
        seg = diff[a:b]
        return round(float(seg.mean()), 6) if seg.size else 0.0

    tr = 0
    for i in range(n - 1, -1, -1):
        if buf[i] == SENT:
            tr += 1
        else:
            break
    idx = np.flatnonzero(diff)
    f_b, f_c = frac(plane3, plane4), frac(plane4, n)
    if f_b < 0.01 and f_c < 0.001:
        verdict = "exactly w*h*3 (BGR24); untouched at byte index >= w*h*3"
    elif f_b > 0.5 and f_c < 0.001:
        verdict = "exactly w*h*4 (RGBA8); untouched at byte index >= w*h*4"
    else:
        verdict = "ambiguous/other"
    return {"mode": mode, "w": w, "h": h, "plane_w3": plane3, "plane_w4": plane4,
            "buffer_bytes": n, "touched_frac_0_to_w3": frac(0, plane3),
            "touched_frac_w3_to_w4": f_b, "touched_frac_w4_to_end": f_c,
            "first_touched_byte": int(idx[0]) if idx.size else -1,
            "last_touched_byte": int(idx[-1]) if idx.size else -1,
            "trailing_sentinel_run": int(tr),
            "written_upper_bound_bytes": int(n - tr), "verdict": verdict}


# ----------------------------------------------------------------- measurement
def measure_resolution(lib, w, h, cfg):
    rec = {"w": w, "h": h}
    rec["init_ms"], actual = engine_init(lib, w, h)
    rec["reported_size"] = [int(actual[0]), int(actual[1])]
    rec["size_matches_request"] = bool(actual[0] == w and actual[1] == h)
    rec["options"] = {"style": DEFAULT_STYLE, "intensity": 1.0, "local_tone": 1.0,
                      "local_struct": 1.0, "skin": 0.0, "auto_mask": 0,
                      "ui_correction": 0}

    out3 = np.zeros((h, w, 3), np.uint8)
    content = {c: make_frames(c, w, h) for c in CONTENTS}

    if cfg["do_size"] and (w, h) in ((1920, 1080), (3840, 2160)):
        rec["size_probe_bgr"] = size_probe(lib, w, h, "bgr",
                                           content[MAIN_CONTENT][0])
        if (w, h) == (1920, 1080):
            # positive control: the RGBA entry point must trip the same sentinel,
            # proving the technique detects a w*h*4 write when one happens.
            rec["size_probe_rgba_control"] = size_probe(lib, w, h, "rgba",
                                                        content[MAIN_CONTENT][0])

    # ---- sync dlssnr2_process, one block per content ------------------------
    rec["sync"] = {}
    for c in CONTENTS:
        rounds = cfg["rounds_main"] if c == MAIN_CONTENT else cfg["rounds_other"]
        count = cfg["frames_main"] if c == MAIN_CONTENT else cfg["frames_other"]
        r = run_sync_rounds(lib, content[c], out3, cfg["warmup"], rounds, count)
        rec["sync"][c] = {"rounds": [[round(x, 4) for x in rr] for rr in r],
                          "stats": stats(merge(r))}

    # ---- style 0 vs 1 probe (does the weight-set hint change GPU cost?) -----
    if (w, h) in STYLE_PROBE_RES:
        probe = {}
        warm = max(4, cfg["warmup"] // 2)
        for st in (DEFAULT_STYLE, 0):
            set_options(lib, style=st)
            r = run_sync_rounds(lib, content[MAIN_CONTENT], out3, warm, 2, 15)
            probe["style_%d" % st] = stats(merge(r))
        set_options(lib, style=DEFAULT_STYLE)
        warmup(lib, content[MAIN_CONTENT], out3, 4)
        a = probe["style_%d" % DEFAULT_STYLE]["median"]
        b = probe["style_0"]["median"]
        probe["delta_style0_minus_style1_ms"] = round(b - a, 3)
        rec["style_probe"] = probe

    # ---- output sanity ------------------------------------------------------
    warmup(lib, content[MAIN_CONTENT], out3, 4)
    lib.dlssnr2_process(ptr(content[MAIN_CONTENT][0]), ptr(out3), 0)
    rec["output_sanity"] = {
        "out_min": int(out3.min()), "out_max": int(out3.max()),
        "out_std": round(float(out3.std()), 3), "out_mean": round(float(out3.mean()), 3),
        "in_std": round(float(content[MAIN_CONTENT][0].std()), 3),
        "degenerate_constant_output": bool(out3.min() == out3.max())}

    # ---- pipelined submit/fetch --------------------------------------------
    if cfg["do_pipeline"]:
        pipe_rounds = [run_pipeline(lib, content[MAIN_CONTENT], out3,
                                    cfg["frames_pipe"]) for _ in range(cfg["rounds_pipe"])]
        sub, fet, itr = [], [], []
        for (s, f, i) in pipe_rounds:
            sub.extend(s); fet.extend(f); itr.extend(i)
        rec["pipeline"] = {
            "rounds": [[round(x, 4) for x in rr[2]] for rr in pipe_rounds],
            "iter_ms": [round(x, 4) for x in itr],
            "iter_stats": stats(itr), "submit_stats": stats(sub),
            "fetch_stats": stats(fet)}
        smed = rec["sync"][MAIN_CONTENT]["stats"]["median"]
        imed = rec["pipeline"]["iter_stats"]["median"]
        rec["gap_sync_minus_pipe_ms"] = round(smed - imed, 3)
        rec["pipeline_speedup_x"] = round(smed / imed, 3) if imed > 0 else None
    return rec


def fmt_row(rec):
    s = rec["sync"][MAIN_CONTENT]["stats"]
    row = {"resolution": "%dx%d" % (rec["w"], rec["h"]), "init_ms": rec["init_ms"],
           "sync_median_ms": s["median"], "sync_mean_ms": s["mean"],
           "sync_p10_ms": s["p10"], "sync_p90_ms": s["p90"],
           "sync_min_ms": s["min"], "sync_n": s["n"]}
    if "pipeline" in rec:
        ps, ss = rec["pipeline"]["iter_stats"], rec["pipeline"]["submit_stats"]
        fs = rec["pipeline"]["fetch_stats"]
        row.update({"pipe_iter_median_ms": ps["median"], "pipe_iter_p90_ms": ps["p90"],
                    "pipe_submit_median_ms": ss["median"],
                    "pipe_fetch_median_ms": fs["median"],
                    "gap_ms": rec["gap_sync_minus_pipe_ms"],
                    "speedup_x": rec["pipeline_speedup_x"]})
    return row


def build_md(res_order, results):
    L = ["# bench_verify.py -- independent DLSSNR per-frame cost verification", "",
         "Host: `%s`" % HOST_DLL, ""]
    L += ["## Method", "",
          "- `dlssnr2_process` (sync) timed with `time.perf_counter` around the call only.",
          "- 4 distinct moving frames per content (temporal denoiser never sees a frozen image).",
          "- Measured frames pass `reset=0` (steady-state playback); warmup frame 0 passes `reset=1`.",
          "- Pipeline measured as fetch-then-submit with both slots primed, matching the 2-slot host.",
          "- NV12<->BGR24 conversion is NOT inside the timer (same as the filter's panel metric).",
          "- Options = filter shipped defaults (style 1, intensity/localtone/localstruct 100%, skin off).", ""]
    L += ["## GPU state during the run (nvidia-smi)", "",
          "```", json.dumps(results["gpu_state"], indent=2), "```", ""]
    if results.get("drift_check"):
        d = results["drift_check"]
        L += ["Drift check (first resolution measured again at the end): **%s** "
              "first pass %.2f ms -> second pass %.2f ms (delta %+.2f ms)" %
              (d["res"], d["first_pass"]["median"], d["second_pass"]["median"], d["delta_ms"]), ""]
    L += ["## Main scaling table (content = %s)" % MAIN_CONTENT, "",
          "| resolution | init ms | sync median | sync mean | sync p10 | sync p90 | "
          "pipe iter median | pipe submit median | pipe fetch median | gap (sync-pipe) | speedup |",
          "|---|---|---|---|---|---|---|---|---|---|---|"]
    for lab in res_order:
        r = results["resolutions"][lab]
        s = r["sync"][MAIN_CONTENT]["stats"]
        if "pipeline" in r:
            ps, ss = r["pipeline"]["iter_stats"], r["pipeline"]["submit_stats"]
            fs = r["pipeline"]["fetch_stats"]
            L.append("| %s | %.0f | %.2f | %.2f | %.2f | %.2f | %.2f | %.2f | %.2f | %.2f | %.2fx |"
                     % (lab, r["init_ms"], s["median"], s["mean"], s["p10"], s["p90"],
                        ps["median"], ss["median"], fs["median"],
                        r["gap_sync_minus_pipe_ms"], r["pipeline_speedup_x"]))
        else:
            L.append("| %s | %.0f | %.2f | %.2f | %.2f | %.2f | - | - | - | - | - |"
                     % (lab, r["init_ms"], s["median"], s["mean"], s["p10"], s["p90"]))
    L += ["", "## Content sensitivity (sync dlssnr2_process, median ms per content)", "",
          "| resolution | " + " | ".join(CONTENTS) + " | spread (max-min) |",
          "|---|" + "---|" * (len(CONTENTS) + 1)]
    for lab in res_order:
        r = results["resolutions"][lab]
        meds = [r["sync"][c]["stats"]["median"] for c in CONTENTS]
        L.append("| %s | " % lab + " | ".join("%.2f" % m for m in meds)
                 + " | %.2f |" % (max(meds) - min(meds)))
    L += ["", "## Style hint probe (style 0 vs shipped default style 1), sync median ms", ""]
    for lab in res_order:
        r = results["resolutions"][lab]
        if "style_probe" in r:
            p = r["style_probe"]
            L.append("- **%s**: style1 %.2f ms, style0 %.2f ms (delta %+.2f ms)"
                     % (lab, p["style_1"]["median"], p["style_0"]["median"],
                        p["delta_style0_minus_style1_ms"]))
    L += ["", "## Output-buffer size probe (sentinel 0x%02X, 1 MiB tail guard)" % SENT, ""]
    for lab in res_order:
        r = results["resolutions"][lab]
        for key in ("size_probe_bgr", "size_probe_rgba_control"):
            if key in r:
                p = r[key]
                L.append("- **%s %s**: touched [0,w*h*3)=%.4f, [w*h*3,w*h*4)=%.6f, "
                         "[w*h*4,end)=%.6f; first=%d last=%d; trailing_sentinel=%d "
                         "=> written<=%d bytes -> **%s**"
                         % (lab, p["mode"], p["touched_frac_0_to_w3"],
                            p["touched_frac_w3_to_w4"], p["touched_frac_w4_to_end"],
                            p["first_touched_byte"], p["last_touched_byte"],
                            p["trailing_sentinel_run"], p["written_upper_bound_bytes"],
                            p["verdict"]))
    L += ["", "## Output sanity", ""]
    for lab in res_order:
        s = results["resolutions"][lab]["output_sanity"]
        L.append("- %s: out std %.2f (in std %.2f), range [%d,%d], degenerate=%s"
                 % (lab, s["out_std"], s["in_std"], s["out_min"], s["out_max"],
                    s["degenerate_constant_output"]))
    with open(OUT_MD, "w", encoding="utf-8") as f:
        f.write("\n".join(L) + "\n")


# ----------------------------------------------------------------------- main
def main(argv=None):
    ap = argparse.ArgumentParser(description="Independent DLSSNR per-frame verifier")
    ap.add_argument("--check-only", action="store_true",
                    help="print the plan and exit WITHOUT loading the engine")
    ap.add_argument("--res", default="all",
                    help="comma list, e.g. 1920x1080,3840x2160")
    ap.add_argument("--warmup", type=int, default=10)
    ap.add_argument("--frames-main", type=int, default=40)
    ap.add_argument("--rounds-main", type=int, default=3)
    ap.add_argument("--frames-other", type=int, default=25)
    ap.add_argument("--rounds-other", type=int, default=2)
    ap.add_argument("--frames-pipe", type=int, default=40)
    ap.add_argument("--rounds-pipe", type=int, default=3)
    ap.add_argument("--no-drift", action="store_true")
    ap.add_argument("--skip-pipeline", action="store_true")
    ap.add_argument("--skip-size", action="store_true")
    ap.add_argument("--quick", action="store_true")
    args = ap.parse_args(argv)
    if args.quick:
        args.warmup = 6
        args.frames_main, args.rounds_main = 20, 2
        args.frames_other, args.rounds_other = 15, 1
        args.frames_pipe, args.rounds_pipe = 20, 2

    if args.res == "all":
        chosen = list(ALL_RES)
    else:
        want = set(x.strip() for x in args.res.split(","))
        chosen = [r for r in ALL_RES if r[0] in want]
        if not chosen:
            ap.error("no known resolution in --res %r" % args.res)

    if args.check_only:
        print("plan     : host=%s" % HOST_DLL)
        print("res      : %s" % ", ".join(c[0] for c in chosen))
        print("sync     : main %s %d rounds x %d frames; others %d x %d; warmup %d"
              % (MAIN_CONTENT, args.rounds_main, args.frames_main,
                 args.rounds_other, args.frames_other, args.warmup))
        print("pipeline : %d rounds x %d frames (fetch-then-submit, 2 slots)"
              % (args.rounds_pipe, args.frames_pipe))
        print("size     : bgr at 1080p+2160p, rgba positive control at 1080p")
        print("style    : probe style 1 vs 0 at 1080p+2160p")
        print("drift    : re-measure first res at the end")
        return 0

    cfg = {"warmup": args.warmup, "frames_main": args.frames_main,
           "rounds_main": args.rounds_main, "frames_other": args.frames_other,
           "rounds_other": args.rounds_other, "frames_pipe": args.frames_pipe,
           "rounds_pipe": args.rounds_pipe, "do_pipeline": not args.skip_pipeline,
           "do_size": not args.skip_size, "do_drift": not args.no_drift}

    lib = load_lib()
    lib.dlssnr2_set_appdir(APP_DIR)          # explicit; must precede first init
    sampler = GpuSampler(0.2)
    sampler.start()
    results = {"meta": {
        "host_dll": HOST_DLL, "host_dll_bytes": os.path.getsize(HOST_DLL),
        "host_dll_mtime": time.strftime("%Y-%m-%d %H:%M:%S",
                                        time.localtime(os.path.getmtime(HOST_DLL))),
        "python": sys.version.split()[0], "numpy": np.__version__,
        "started": time.strftime("%Y-%m-%d %H:%M:%S"), "main_content": MAIN_CONTENT,
        "argv": sys.argv[1:],
        "note": ("sync timer covers only the dlssnr2_process call; reset=0 on every "
                 "measured frame; NV12<->BGR24 conversion not included")},
        "resolutions": {}}
    res_order = []
    try:
        for label, w, h in chosen:
            print("[%s] init + measure ..." % label, flush=True)
            wall0 = time.time()
            rec = measure_resolution(lib, w, h, cfg)
            rec["gpu"] = sampler.report(wall0, time.time())
            results["resolutions"][label] = rec
            res_order.append(label)
            s = rec["sync"][MAIN_CONTENT]["stats"]
            extra = ""
            if "pipeline" in rec:
                extra = " pipe=%.2f ms (gap %.2f)" % (
                    rec["pipeline"]["iter_stats"]["median"], rec["gap_sync_minus_pipe_ms"])
            print("    init %.0f ms  sync median %.2f ms (n=%d)%s"
                  % (rec["init_ms"], s["median"], s["n"], extra), flush=True)
        if cfg["do_drift"] and res_order:
            lab = res_order[0]
            w, h = chosen[0][1], chosen[0][2]
            print("[drift] re-measuring %s at the end ..." % lab, flush=True)
            engine_init(lib, w, h)
            frames = make_frames(MAIN_CONTENT, w, h)
            out3 = np.zeros((h, w, 3), np.uint8)
            rr = run_sync_rounds(lib, frames, out3, cfg["warmup"], 1, cfg["frames_main"])
            s2 = stats(merge(rr))
            s1 = results["resolutions"][lab]["sync"][MAIN_CONTENT]["stats"]
            results["drift_check"] = {"res": lab, "second_pass": s2, "first_pass": s1,
                                      "delta_ms": round(s2["median"] - s1["median"], 3)}
            print("    first pass %.2f ms -> second pass %.2f ms (delta %+.2f ms)"
                  % (s1["median"], s2["median"], results["drift_check"]["delta_ms"]),
                  flush=True)
    except Exception:
        import traceback
        results["error"] = traceback.format_exc()
    try:
        sampler.stop()
    except Exception:
        pass
    results["gpu_state"] = sampler.report()
    results["summary_table"] = [fmt_row(results["resolutions"][l]) for l in res_order]

    with open(OUT_JSON, "w", encoding="utf-8") as f:
        json.dump(results, f, indent=2)
    build_md(res_order, results)

    print()
    print("[summary] content=%s, options=shipped defaults" % MAIN_CONTENT)
    print("%-10s %8s %10s %10s %10s %10s %8s" %
          ("res", "init_ms", "sync_ms", "pipe_ms", "submit_ms", "fetch_ms", "gap_ms"))
    for row in results["summary_table"]:
        g = lambda k, f="%.2f": (f % row[k]) if k in row else "-"
        print("%-10s %8.0f %10.2f %10s %10s %10s %8s"
              % (row["resolution"], row["init_ms"], row["sync_median_ms"],
                 g("pipe_iter_median_ms"), g("pipe_submit_median_ms"),
                 g("pipe_fetch_median_ms"), g("gap_ms")))
    print()
    print("raw JSON: %s" % OUT_JSON)
    print("report  : %s" % OUT_MD)
    if "error" in results:
        print("ERROR during run:\n%s" % results["error"])
        return 2
    return 0


if __name__ == "__main__":
    sys.exit(main())
