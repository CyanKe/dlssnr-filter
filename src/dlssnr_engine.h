// dlssnr_engine.h -- the filter's own D3D12 + NGX engine.
//
// This repository used to dlopen an external host (dlssnr_host2.dll, built from
// the separate dlssnr-toolkit repository) and hand it CPU-converted BGR24. That
// meant every frame paid for NV12 -> BGR24 -> RGBA8 and back, all on the CPU:
// measured 48 ms/frame of pure colour conversion at 3840x2076, more than the
// neural pass itself.
//
// This engine keeps everything on the GPU instead:
//
//     NV12 planes --(upload)--> [compute: NV12 -> RGBA8] --> NGX --> [compute:
//     RGBA8 -> NV12] --(readback)--> NV12 planes
//
// The CPU now only copies two planes in and two planes out (~2.6 ms at 4K).
//
// The NGX entry points are resolved from nvngx_dlssnr.dll at runtime, the same
// way dlssnr_host2.dll does it: that DLL exports Init_Ext / CreateFeature /
// EvaluateFeature / ReleaseFeature, while the SDK's static library supplies
// GetCapabilityParameters / DestroyParameters. See deps\README.md.
// ---------------------------------------------------------------------------
// Engineering constraints this engine lives under. These used to be written
// down in docs/ENGINE_INTERFACE.md, which documented a binary contract between
// two repositories -- that contract is gone (the engine is in-process now) but
// the constraints are not:
//
//  * NGX may only be initialised ONCE PER PROCESS, so Shutdown() releases the
//    feature and the GPU resources but keeps the D3D12 device and the NGX core
//    alive. A player builds a brand-new filter instance for every file it
//    opens, which is exactly why this cannot be per-instance.
//  * nvngx_dlssnr.dll must STAY LOADED for the life of the process. Its
//    "NGX already inited" latch lives inside its own DLL image; unloading it
//    would clear that latch while NGX itself stays initialised, and the next
//    Init_Ext then fails with 0xBAD00002 until the player is restarted. We
//    never FreeLibrary it.
//  * Changing resolution in-process is supported: Init() at a new size reuses
//    the device and rebuilds only the NGX feature and the frame resources.
//  * During Init_Ext the snippet asks Windows for the file name of its CALLER's
//    module and refuses to run unless it reads "nvngx.dll", so the engine
//    installs an IAT shim -- see InstallCallerShim() in the .cpp.
//  * d3dcompiler_47.dll is loaded dynamically on purpose: if it is missing the
//    engine simply reports itself unavailable (and the filter stays a pure
//    pass-through) instead of preventing the filter DLL from loading at all.
// ---------------------------------------------------------------------------
#pragma once
#include <windows.h>
#include <stdint.h>

namespace dlssnr {

struct Options {
    int   style        = 0;
    float intensity    = 1.0f;
    float localTone    = 1.0f;
    float localStruct  = 1.0f;
    float skinStruct   = 0.0f;
    int   autoMask     = 0;
    int   uiCorrection = 0;
};

// Creates the device and the NGX feature at this geometry. `moduleDir` must be
// the folder holding nvngx_dlssnr.dll (i.e. the filter DLL's own directory).
// Returns false on any failure; the filter then stays in pass-through.
bool Init(int width, int height, const wchar_t* moduleDir, const char* logPath);

// Drops the GPU resources but keeps the D3D12 device and the NGX core alive --
// re-initialising NGX a second time in one process is not safe, and a player
// builds a new filter instance for every file it opens. Safe to call twice.
void Shutdown();

bool Ready();
int  Width();
int  Height();

void SetOptions(const Options& o);

// One frame, tightly packed or strided NV12 in / NV12 out. Blocking.
bool ProcessNv12(const uint8_t* inY,  int inYStride,
                 const uint8_t* inUV, int inUVStride,
                 uint8_t* outY,       int outYStride,
                 uint8_t* outUV,      int outUVStride,
                 bool reset);

} // namespace dlssnr
