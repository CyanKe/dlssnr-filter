# Independent verification of DLSSNR per-frame cost (task-2)

Author: bench-verifier.  Harness: `tools/bench_verify.py`, written from
`docs/ENGINE_INTERFACE.md` and `src/dlssnr_host2.cpp` only; no code shared with
`tools/bench_engine.py`.

> Note (lead): the four one-off probe scripts the verifier used for the follow-up
> experiments (`_focus`, `_clocktest`, `_paths`, `_stateprobe`) were removed after
> their results were folded into this document, to keep `tools/` down to the two
> re-runnable harnesses. Their conclusions are sections 3-6 below; the warm-state
> and cold-state numbers are also reproducible from the shipped harnesses
> (`tools/bench_engine.py` for the current one).
Host under test: `D:\VSCODE\dlssnr-filter\app\dlssnr_host2.dll` (loads nvngx next to it).

## Bottom line

1. The Lead's sync `dlssnr2_process` numbers are **reproduced within 0.1-1.3 ms (<=5%)**
   in the same GPU power state the Lead measured ("state A").  See the main table.
2. That state is **not stable**.  The same machine, same binary, same content, later ran
   the identical loop ~1.3-1.5x faster ("state B").  So the clock cap the Lead saw is
   real but transient, not a fixed hardware pin.  A README table needs to state the
   power state or quote both.
3. `dlssnr2_process` output is **exactly `w*h*3` BGR24** (sentinel probe, with a positive
   RGBA control).  `docs/ENGINE_INTERFACE.md:152-153` (`w*h*4` RGBA8) is wrong; the
   prose at `:39-42` and the host source are right.
4. Per-frame cost is **content-insensitive** (<0.5 ms spread flat/noise/detail at every
   resolution in state B; <1.3 ms at 1440p/2160p in state A).  The one 1080p outlier in
   state A was a clock-state shift, not content.
5. The pipelined `submit`/`fetch` path is faster than sync in both states (1.1-1.25x in
   state B, 1.35-1.65x in state A) - a real optimisation opportunity for the filter.

## 1. Main table - sync dlssnr2_process, content = noise, reset = 0

State A = the Lead's measured power state (SM ~1.2 GHz under load, mem ~13-14 GHz).
State B = the later observed state (SM ~2.2-2.4 GHz, mem ~9 GHz).
My state A numbers are run 2 of `bench_verify.py` (init warm, median of 120 frames).

| resolution | Lead median ms | my state A ms | delta ms | delta % | my state B ms | B vs A |
|---|---|---|---|---|---|---|
| 1280x720   | 8.84 | 8.71 (drift re-measure; first block 6.25) | -0.13 | -1.5% | ~5.6-6.0 | 1.5x |
| 1920x1080  | 15.96 | 16.09 | +0.13 | +0.8% | ~10.5 | 1.5x |
| 2560x1440  | 25.70 | 24.46 | -1.24 | -4.8% | ~18.0 | 1.4x |
| 3840x2160  | 55.41 | 54.38 | -1.03 | -1.9% | ~43.3 | 1.3x |

- The 720p first block in run 2 was 6.25 ms because the governor was still un-capped at
  the very start of the process; the end-of-run drift re-measure gave 8.71 ms, which is
  the comparable number.  (My spin-up drift was 6.25 -> 8.71; the Lead's was 8.84 -> 8.85.)
- First-frame `reset=1`: see section 5.

## 2. GPU state actually observed

State A (`bench_verify_results.json`, run 2), per-resolution nvidia-smi window
(min-max / mean):

| resolution | SM MHz | mem MHz | power W | temp C |
|---|---|---|---|---|
| 1280x720  | 450-2392 / 1421 | 405-14001 / 12402 | 12-108 / 53 | 44-55 |
| 1920x1080 | 405-2205 / 1243 | 9001-14001 / 13287 | 17-104 / 61 | 45-59 |
| 2560x1440 | 240-2107 / 1247 | 9001-14001 / 13223 | 15-131 / 67 | 47-64 |
| 3840x2160 | 225-2107 / 1177 | 9001-14001 / 13216 | 15-130 / 66 | 49-66 |

State B (`bench_verify_clocktest.json`, 400-frame continuous 4K loop, per-40-frame
buckets): flat 41.5-43.8 ms across all 10 buckets, SM 2212-2426 MHz, power 112-137 W;
the sampler-OFF control was 43.13 ms vs 42.81 ms with the sampler, so nvidia-smi polling
is not what keeps clocks up.  `enforced.power.limit` read **130 W** in state A and
**140 W** in state B.

Verdict on the clock cap: **confirmed as a state, refuted as a pin.**  In state A the
under-load SM clock really is ~1.2 GHz (max 2107-2392) with power mostly 15-67 W against a
130 W limit - exactly the Lead's observation, and not thermal (<=66 C).  But with no user
action the same process later held 2200-2600 MHz / 130-136 W for 400 consecutive frames.

Two follow-ups matter for the "cap":
- The split is complementary: state A = high mem clock (13-14 GHz) + low SM; state B =
  low mem (9 GHz) + high SM.  The 4K path is bandwidth-heavy, so the *pipeline* is
  actually faster in state A (34.3 ms) than in state B (39.0 ms) even though sync is
  slower (54.4 vs 43.3).
- Because of that, the documented toolkit fit (2.5 + 4.4*MPix: 11.6 ms @1080p, 39.0 ms
  @4K) matches **state B**, not state A.  The Lead's ~1.45x-above-fit observation is
  therefore a state-A artefact.

## 3. Sync vs pipelined submit/fetch

Pipeline loop = prime both slots, then fetch-then-submit (submit-then-fetch just drains
every step and returns 0 at pending==2).  `iter` = fetch(wait for oldest) + submit.

State A (`bench_verify_results.json`, 3x40 frames):

| resolution | sync med ms | pipe iter med ms | pipe submit med ms | pipe fetch med ms | gap ms | speedup |
|---|---|---|---|---|---|---|
| 1280x720  | 6.25 | 7.30 | 0.92 | 6.30 | -1.05 | 0.86x |
| 1920x1080 | 16.09 | 11.95 | 1.31 | 10.68 | 4.14 | 1.35x |
| 2560x1440 | 24.46 | 14.84 | 2.01 | 12.74 | 9.63 | 1.65x |
| 3840x2160 | 54.38 | 34.32 | 3.75 | 30.57 | 20.05 | 1.58x |

State B (`bench_verify_paths.json` / `bench_verify_stateprobe.py`, alternating phases in
one process):

| resolution | sync med ms | pipe iter med ms | speedup |
|---|---|---|---|
| 1280x720  | 5.55 | 4.50 | 1.23x |
| 1920x1080 | 10.5-11.6 | 8.4-8.6 | 1.25-1.34x |
| 2560x1440 | 17.98 | 15.16 | 1.19x |
| 3840x2160 | 43.3 | 39.0 | 1.11x |

So the pipeline hides the CPU staging/record work (submit ~0.8-3.8 ms) and, in state A,
also keeps the GPU in a better state; the win shrinks once the SM clock is already high.
It is never worse than sync beyond noise (the 720p state-A 0.86x is within run noise).

## 4. Content sensitivity

State B (`bench_verify_focus.py`, 5 blocks x 20 frames per content, rotated order,
median ms):

| resolution | noise | flat | detail | spread |
|---|---|---|---|---|
| 1280x720  | 5.97 | 5.91 | 5.93 | 0.06 |
| 1920x1080 | 10.90 | 10.71 | 10.76 | 0.19 |
| 2560x1440 | 18.02 | 17.64 | 17.70 | 0.38 |
| 3840x2160 | 40.23 | 39.71 | 39.73 | 0.52 |

State A (`bench_verify_results.json`, 2x25 frames for flat/gradient/detail):

| resolution | flat | gradient | noise | detail | spread |
|---|---|---|---|---|---|
| 1280x720  | 7.04 | 6.12 | 6.25 | 8.82 | 2.70 |
| 1920x1080 | 16.01 | 16.10 | 16.09 | 10.35 | 5.75 |
| 2560x1440 | 25.56 | 25.73 | 24.46 | 25.38 | 1.27 |
| 3840x2160 | 55.19 | 54.46 | 54.38 | 54.14 | 1.04 |

Conclusion: **cost is essentially content-independent** (<=2% of the median except 720p
where the absolute cost is tiny).  The 1080p detail=10.35 ms cell is a state shift: in
state B the whole 1080p level is ~10.5-10.9 ms regardless of content, so that block simply
caught the faster clock.  Flat vs pure noise at 1080p differ by <=0.2 ms in state B.

## 5. reset = 1 (session start / seek)

Measured (ms): 1280x720 5.57-5.88, 1920x1080 10.79-12.12, 2560x1440 17.49-23.83,
3840x2160 42.09-46.08.

These are **not consistently above** the reset=0 medians (ratios 0.93-1.32, and the
1440p/4K values straddle the steady numbers).  The Lead's uniformly higher reset=1 cost
(+18% to +84%) did **not** reproduce; in my runs the reset penalty is inside the clock-state
noise, so I can neither confirm nor deny a systematic reset cost.

## 6. Output-buffer size probe (sentinel 0xA5, 1 MiB tail guard)

`dlssnr2_process` (BGR path), oversized buffer filled with sentinel:

| res | touched [0,w*h*3) | touched [w*h*3,w*h*4) | touched [w*h*4,end) | last touched byte | trailing sentinel | written <= |
|---|---|---|---|---|---|---|
| 1920x1080 | 0.9946 | 0.000000 | 0.000000 | 6,220,799 | 3,122,176 | 6,220,800 = w*h*3 |
| 3840x2160 | 0.9948 | 0.000000 | 0.000000 | 24,883,199 | 9,342,976 | 24,883,200 = w*h*3 |

Positive control, `dlssnr2_process_rgba` at 1920x1080 into the same sentinel buffer:
touched [w*h*3,w*h*4) = **0.996112**, touched [w*h*4,end) = 0.000000, written <=
8,294,400 = w*h*4 - i.e. the probe does detect a `w*h*4` write when one happens.

Verdict: **`dlssnr2_process` writes exactly `w*h*3` bytes of BGR24.**  `docs/ENGINE_INTERFACE.md`
line 153 (`w*h*4` RGBA8 for `dlssnr2_process`) is wrong; lines 39-42 are correct; this also
matches the host source (`FetchCore`, `src/dlssnr_host2.cpp:988-1006`) and the filter's
`w*3*h` scratch (`src/dlssnr_dshow.cpp:2073-2075`).

## 7. Method notes / what is and is not timed

- Timer wraps only the `dlssnr2_process` call (`time.perf_counter`), exactly like the
  filter panel metric `lastProcessMs`.  NV12<->BGR24, decode, DirectShow plumbing excluded.
- 4 distinct moving frames per content; warmup frame 0 uses reset=1, measured frames reset=0.
- Options = filter shipped defaults (style 1, intensity/tone/struct 1.0, skin 0).
- Style probe (style 0 vs 1): 1080p 14.18 vs 11.83 ms, 4K 53.20 vs 54.05 ms - no consistent
  effect, both inside the state noise, so the option choice does not explain the numbers.
- Output sanity: processed frames are non-degenerate (out std ~53-54, range [2,234]).
- Init was 935-973 ms cold / 120-144 ms per size re-init.

## 8. Artefacts

- `tools/bench_verify.py` - main harness (+ `bench_verify_results.json`, `bench_verify_report.md`)
- `tools/bench_verify_focus.py` / `.json` - content sensitivity + reset=1
- `tools/bench_verify_clocktest.py` / `.json` - 400-frame continuous loop, state check
- `tools/bench_verify_paths.py` / `.json` - same-state sync vs pipeline
- `tools/bench_verify_stateprobe.py` / `.json` - alternating sync/pipeline + nvidia-smi snapshots
- `tools/bench_verify_findings.md` - this file

No engine/GPU work was done before the Lead's "GPU is free" message.  README.md and
`tools/bench_engine.py` were not modified.
