"""Read the DLSSNR filter's live telemetry out of its named shared memory.

The filter publishes SharedState to Local\DLSSNR_DShow_Shared_v1 every frame
(see src/dlssnr_dshow.cpp:123-157 for the layout, 364 bytes packed). This lets
us watch a REAL playback session from outside the player: what the engine costs
per frame, and how many frames it processed vs passed through.
"""
import ctypes, struct, sys, time
from ctypes import wintypes

NAME = "Local\\DLSSNR_DShow_Shared_v1"
FILE_MAP_READ = 0x0004

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
p = k32.MapViewOfFile(h, FILE_MAP_READ, 0, 0, 364)
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
print("t     hb  seen/processed/pass  video   engine  bpp  ms/frame  status")
prev = None
t0 = time.time()
try:
    while True:
        hb = i32(16)
        if hb != prev:
            prev = hb
            print("%5.1fs %4d  %6d/%-6d/%-6d %4dx%-4d %4dx%-4d %d  %8.2f  %s" % (
                time.time() - t0, hb, i32(20), i32(24), i32(28),
                i32(48), i32(52), i32(40), i32(44), i32(56), f32(64), status()))
        sys.stdout.flush()
        time.sleep(0.4)
except KeyboardInterrupt:
    pass
finally:
    k32.UnmapViewOfFile(ctypes.c_void_p(p))
    k32.CloseHandle(h)
