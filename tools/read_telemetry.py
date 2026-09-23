"""Read the DLSSNR filter's live telemetry out of its named shared memory.

The filter publishes SharedState to Local\DLSSNR_DShow_Shared_v2 every frame
(see src/dlssnr_dshow.cpp -- the struct comment has the layout, 380 bytes
packed). This lets us watch a REAL playback session from outside the player:
what the engine costs per frame, how many frames it processed vs passed through,
and how many the late-frame policy dropped to stay in sync.

That policy trades frame rate for sync, so the interesting numbers are the two
rates: "in" is what the source offers, "out" is what reaches the renderer. On a
source the engine cannot keep up with (4K60 against a 34 ms engine) expect
out ~= 1000 / ms-per-frame and most of the rest dropped; when the engine keeps
up, out == in and dropped stays at 0.

Run: python tools\read_telemetry.py      (Ctrl+C to stop)
"""
import ctypes, struct, sys, time
from ctypes import wintypes

NAME = "Local\\DLSSNR_DShow_Shared_v2"
FILE_MAP_READ = 0x0004
MAP_SIZE = 380              # sizeof(SharedState): the v2 layout, #pragma pack(1)

k32 = ctypes.WinDLL("kernel32", use_last_error=True)
k32.OpenFileMappingW.argtypes = [wintypes.DWORD, wintypes.BOOL, wintypes.LPCWSTR]
k32.OpenFileMappingW.restype = wintypes.HANDLE
k32.MapViewOfFile.argtypes = [wintypes.HANDLE, wintypes.DWORD, wintypes.DWORD,
                              wintypes.DWORD, ctypes.c_size_t]
k32.MapViewOfFile.restype = ctypes.c_void_p
k32.UnmapViewOfFile.argtypes = [ctypes.c_void_p]
k32.CloseHandle.argtypes = [wintypes.HANDLE]

h = k32.OpenFileMappingW(FILE_MAP_READ, False, NAME)
if not h:
    print("shared memory %r not found (filter not running)" % NAME)
    sys.exit(2)
p = k32.MapViewOfFile(h, FILE_MAP_READ, 0, 0, MAP_SIZE)
if not p:
    print("MapViewOfFile failed")
    sys.exit(3)


def u32(off):
    return struct.unpack_from("<I", ctypes.string_at(p + off, 4))[0]


def i32(off):
    return struct.unpack_from("<i", ctypes.string_at(p + off, 4))[0]


def f32(off):
    return struct.unpack_from("<f", ctypes.string_at(p + off, 4))[0]


def status(off=80):
    return ctypes.wstring_at(p + off, 128).split("\x00")[0]


print("magic=0x%08X version=%d structSize=%d" % (u32(0), u32(4), u32(8)))
print("   t     in-fps  out-fps   seen    proc    pass   drop   drop%  ms/frame  lag ms  sw  status")
prev = None
prev_t = prev_seen = prev_out = None
t0 = time.time()
try:
    while True:
        hb = i32(16)
        now = time.time()
        if hb != prev:
            prev = hb
            seen, proc, pas = i32(20), i32(24), i32(28)
            drop = i32(364)
            out = proc + pas
            if prev_t is not None and now > prev_t:
                dt = now - prev_t
                in_fps = (seen - prev_seen) / dt
                out_fps = (out - prev_out) / dt
            else:
                in_fps = out_fps = 0.0
            prev_t, prev_seen, prev_out = now, seen, out
            print("%6.1fs %7.1f %7.1f %6d %7d %7d %6d  %5.1f%% %8.2f %7.1f  %s  %s" % (
                now - t0, in_fps, out_fps, seen, proc, pas, drop,
                f32(372), f32(64), f32(376),
                "on " if i32(368) else "off", status()))
        sys.stdout.flush()
        time.sleep(0.4)
except KeyboardInterrupt:
    pass
finally:
    k32.UnmapViewOfFile(ctypes.c_void_p(p))
    k32.CloseHandle(h)