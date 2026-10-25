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

#include "dlssnr_layers.h"     // kMaxLayers: the ceiling on layers per frame

namespace dlssnr {

// One layer, in the engine's own units (floats; the panel's percent values are
// converted to these once, in the Engine wrapper).
struct LayerOptions {
    int   style        = 0;
    float intensity    = 1.0f;
    float localTone    = 1.0f;
    float localStruct  = 1.0f;
    float skinStruct   = 0.0f;
    int   autoMask     = 0;
    int   uiCorrection = 0;
};

// The whole frame's settings: N layers, applied in order 0 -> layerCount-1.
//
// Every layer runs on its OWN NGX feature, so every layer owns its own temporal
// history. That is not an optimisation -- see the long note in dlssnr_layers.h
// (and CreateFeatureForLayer() in the .cpp): putting two passes on one feature
// doubles the temporal loop gain and makes flat areas pulse at the frame rate.
// One feature per layer is what keeps layer 2 well-behaved.
//
// The whole chain is recorded into ONE command list, so a frame still costs one
// upload, one fence wait and one readback however many layers are in it.
struct Options {
    LayerOptions layers[kMaxLayers];
    int          layerCount = 1;   // 1..kMaxLayers (SetOptions clamps)

    // Extrapolation gain, in panel percent: 100 = OFF, 180 = 1.8x. Applied ONCE
    // at the end of the chain, not per layer:
    //
    //     out = proc + (gain - 1) * (proc - in)
    //
    // where `in` is the frame as uploaded and `proc` is what the chain produced.
    // This is NOT a layer and must not be labelled as one: it re-uses one pass's
    // correction instead of running another. Measured on real footage, the
    // per-layer corrections are almost collinear (cos 0.988/0.966/0.926) but
    // DECAY (0.771/0.591/0.461), so the gain that emulates N layers is the
    // geometric sum 1.80/2.40/2.85, never N itself -- k=N overshoots
    // systematically. Clamped to [100, 400] by SetOptions.
    int          extrapPct = 100;
};

// Creates the device and the NGX feature at this geometry. `moduleDir` must be
// the folder holding nvngx_dlssnr.dll (i.e. the filter DLL's own directory).
// Returns false on any failure; the filter then stays in pass-through.
//
// `scalePercent` is the size the NETWORK runs at, as a percentage of the video
// size, clamped to [25, 100]; 100 (the default) is native. The frame that goes
// in and the frame that comes out are ALWAYS at the video size -- only the
// neural pass and the two colour conversions feeding it shrink, so no caller has
// to know. Down is an area average (no aliasing), up is bilinear (soft). Asking
// for a size that is too small, or for which the feature cannot be created,
// silently falls back to native; ProcScalePercent() reports what was APPLIED, so
// callers can tell the user the truth.
//
// `modelPreset` is WHICH network inside nvngx_dlssnr.dll to run, written into the
// creation parameter "DLSSNR.Hint.Render.Preset" (see src\dlssnr_models.h for
// what the values mean and how they were measured). 0 = the DLL's own default.
// It is a feature-CREATION parameter, so a different value means different
// features: Init() releases the old set and creates the new one, exactly as it
// already does for a geometry or scale change. A preset the DLL refuses does NOT
// cost the picture -- the engine retries with 0 and ModelFellBack() says so.
bool Init(int width, int height, const wchar_t* moduleDir, const char* logPath,
          int scalePercent = 100, int modelPreset = 0);

// Drops the GPU resources but keeps the D3D12 device and the NGX core alive --
// re-initialising NGX a second time in one process is not safe, and a player
// builds a new filter instance for every file it opens. Safe to call twice.
void Shutdown();

bool Ready();
int  Width();              // the video size: what ProcessNv12 takes and returns
int  Height();
int  ProcWidth();          // the size the network runs at (== Width() at 100%)
int  ProcHeight();
int  ProcScalePercent();   // 100 or 50, as applied (may have fallen back to 100)
int  ActiveLayers();       // NGX features live right now (grows on demand)

// Which network the live features were created with, as APPLIED: 0 = the DLL's
// own default. This can differ from what Init() was asked for only when the DLL
// refused the request, which ModelFellBack() reports.
int  ModelPreset();
bool ModelFellBack();

// Tells the engine where nvngx_dlssnr.dll lives, WITHOUT initialising anything.
// Init() sets this too, but the control panel can be opened before a single frame
// has been played (the property page, the tray), and the capability scan below
// needs the folder to answer "which networks does this DLL have?" -- with no
// folder the scan returns nothing and the panel would grey out choices that are
// perfectly available. Safe to call from DllMain: it only copies a path.
void SetModuleDir(const wchar_t* dir);

// ---- what the loaded nvngx_dlssnr.dll can actually serve -------------------
//
// The DLL is the only authority on which networks it holds, so this list is read
// out of the DLL's own image -- the NUL-terminated WEIGHTS_* names in its .rdata
// -- rather than inferred from the file version. That is what makes a DLL this
// build has never seen selectable: the panel offers a labelled choice for every
// name dlssnr_models.h knows, and a raw preset id for the rest.
//
// The scan loads the DLL if it is not loaded yet (the engine never unloads it,
// so this is the same handle TryInitNgx() would use) and its result is cached.
enum { kDllWeightMax = 8, kDllWeightNameMax = 48 };
struct DllWeight {
    char name[kDllWeightNameMax];   // "WEIGHTS_HT", "WEIGHTS_CC_SMALL_FP8", ...
    int  preset;                    // known preset id, or -1 when unknown here
};
// Returns how many were written (0 = the DLL is missing or has no WEIGHTS_* name).
int  DllWeights(DllWeight* out, int cap);

// The loaded DLL's file version as "310.9.16.0", or "" when it cannot be read.
void DllVersion(wchar_t* out, int cch);

// Stores the settings AND pushes them onto the live features. Layer count is
// clamped to 1..kMaxLayers. Raising it does NOT create the new features here
// (feature creation needs the queue and a command list, and this can be called
// from the panel at any time) -- ProcessNv12 grows the set on the next frame,
// which is the same lazy point the toolkit's chain() used.
void SetOptions(const Options& o);

// One frame, tightly packed or strided NV12 in / NV12 out. Blocking.
bool ProcessNv12(const uint8_t* inY,  int inYStride,
                 const uint8_t* inUV, int inUVStride,
                 uint8_t* outY,       int outYStride,
                 uint8_t* outUV,      int outUVStride,
                 bool reset);

} // namespace dlssnr
