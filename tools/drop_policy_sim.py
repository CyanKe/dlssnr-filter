"""Offline check of the filter's late-frame policy (ShouldDropLateFrame in
src/dlssnr_dshow.cpp).

The real thing needs a GPU and a 4K60 file, and the failure mode the policy
prevents -- a lateness that GROWS -- only shows up after tens of seconds. This
models the pipeline instead:

  * the upstream hands over frame k either exactly when it is due (paced, i.e.
    a source that is clocked) or as fast as we accept it (bursty, i.e. a decoder
    that runs ahead until the renderer's queue pushes back);
  * one frame costs `engine_ms` of engine time, paid on the source's thread;
  * the policy decides BEFORE that cost is paid, exactly like the filter does.

What is checked: the policy has to hold the picture-to-sound offset bounded at
about one engine run while the input rate is higher than 1000/engine_ms. With
the policy off the same run drifts without limit -- that is the bug it fixes.

Run: python tools/drop_policy_sim.py
"""

FRAMES = 600

# Same constants as the filter.
MAX_CONSEC_DROPS = 12
REANCHOR_MS      = 2000.0


def run(interval_ms, engine_ms, bursty=False, drop=True, frames=FRAMES):
    seed_valid = False
    seed_clock = seed_stream = 0.0
    min_lag = 0.0
    consec = 0
    last_ts = None
    frame_interval = 0.0
    t_out = 0.0
    dropped = 0
    offsets = []

    for k in range(frames):
        ts = k * interval_ms                       # presentation (stream) time
        avail = 0.0 if bursty else ts              # when the frame is ready
        t_in = max(avail, t_out)                   # when Receive() is entered
        now = t_in

        if last_ts is not None and ts > last_ts:
            d = ts - last_ts
            if 1.0 <= d <= 1000.0:                 # ignore seek-sized gaps
                frame_interval = d if frame_interval <= 0 else (frame_interval * 7 + d) / 8
        last_ts = ts
        iv = frame_interval if frame_interval > 0 else interval_ms

        if not seed_valid:
            seed_valid, seed_clock, seed_stream, min_lag, consec = True, now, ts, 0.0, 0
            drop_it = False
        else:
            lag = (now - seed_clock) - (ts - seed_stream)
            min_lag = min(min_lag, lag)
            rel = lag - min_lag
            if rel > REANCHOR_MS:                  # clock jumped: re-anchor
                seed_valid, seed_clock, seed_stream, min_lag, consec = True, now, ts, 0.0, 0
                drop_it = False
            elif consec >= MAX_CONSEC_DROPS:       # starvation guard
                drop_it = False
            else:
                cost = engine_ms
                budget = cost + iv                 # maxlagms=0 -> auto
                drop_it = bool(drop and (rel + cost > budget))

        if drop_it:
            dropped += 1
            consec += 1
            t_out = t_in                           # a dropped frame costs nothing
        else:
            consec = 0
            t_out = t_in + engine_ms
            offsets.append(t_out - ts)             # how late it reaches the renderer

    return dropped, offsets


def report(name, interval_ms, engine_ms, bursty=False):
    in_fps = 1000.0 / interval_ms
    for drop in (True, False):
        dropped, offsets = run(interval_ms, engine_ms, bursty, drop)
        out_fps = len(offsets) * 1000.0 / (FRAMES * interval_ms)
        first = offsets[0] if offsets else float("nan")
        last  = offsets[-1] if offsets else float("nan")
        tag = "drop ON " if drop else "drop OFF"
        print("%-14s %s  in %5.1f fps  out %5.1f fps  dropped %3d (%4.1f%%)  "
              "offset first %7.1f ms last %9.1f ms"
              % (name, tag, in_fps, out_fps, dropped, 100.0 * dropped / FRAMES, first, last))
    print()


if __name__ == "__main__":
    print("source         policy   rate in/out                      A/V offset\n"
          + "-" * 108)
    # name, frame interval, engine cost (README measurements)
    report("1080p60",   1000.0 / 60,  9.6)
    report("1440p60",   1000.0 / 60, 17.7)
    report("2560x1440p120", 1000.0 / 120, 17.7)
    report("4K24",      1000.0 / 24, 34.7)
    report("4K60",      1000.0 / 60, 34.7)
    print("bursty upstream (a decoder that runs ahead instead of being clocked):")
    report("4K60 bursty", 1000.0 / 60, 34.7, bursty=True)