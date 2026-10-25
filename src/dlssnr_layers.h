// ===========================================================================
//  dlssnr_layers.h -- the layered-parameter model.
//
//  A "layer" is one more full DLSSNR pass over the previous pass output. The
//  point of this header is that ONE definition of that model is shared by every
//  part that has to agree on it: the ini reader (ReadConfig), the ini writer
//  (the control panel), the settings/tray UI and the engine wrapper.
//
//  WHY A LAYER NEEDS ITS OWN NGX FEATURE
//    A layer is not "the same pass, repeated". Every layer gets its OWN NGX
//    feature, because a feature owns its temporal history: running one feature
//    N times per frame makes the 2nd pass read a history that is bit-identical
//    to its own input, i.e. a degenerate (input, history) pair that says
//    "nothing moved, trust the history completely". That doubles the loop gain
//    of the temporal recursion, and a nonlinear recursion with doubled gain
//    oscillates at the frame rate -- the flat areas pulse. Measured in
//    dlssnr-toolkit on a 70%-flat sequence: stage-mean level instability
//    0.091 (1 layer) -> 0.223 (2 layers on one feature) static, and
//    0.139 -> 0.531 with slow motion inside the flat area. Independent features
//    restore a proper (input, history) pair per layer. See also the note in
//    src/dlssnr_engine.cpp, which is where the chain is actually recorded.
//
//  UNITS
//    Percent, exactly the control panel's native units (100 = 1.0). The engine
//    wants floats, so the conversion happens exactly once -- in the Engine
//    wrapper in src/dlssnr_dshow.cpp. Keeping ints here means the ini, the
//    sliders and the tray presets all speak the same language and nothing can
//    round twice.
// ===========================================================================
#pragma once

// Self-sufficient on purpose: LayerKeyName() uses _snwprintf_s, and this header
// is included from the engine header as well as from the filter, so it cannot
// rely on the including translation unit having pulled <stdio.h> in first.
#include <stdio.h>
#include <wchar.h>

namespace dlssnr {

// Hard ceiling, and the same one dlssnr-toolkit ships. Cost is linear in N
// (each layer is a full neural pass), so 2-3 layers is the realistic range;
// the ceiling exists so a hand-edited ini cannot ask for 1000 features.
enum { kMaxLayers = 8 };

// One layer's parameters, in panel units. Plain ints: copyable, clammable and
// writable to the ini with no unit conversion anywhere.
struct LayerCfg {
    int style;         // 0..3 (see kDlssNrStyleNames)
    int pctIntensity;  // 0..100  -- DLSSNR.Intensity saturates at 1.0
    int pctTone;       // 0..200  -- still changing at 2.0 (measured)
    int pctStruct;     // 0..200
    int pctSkin;       // 0..200  -- 0 = off; needs autoMask to do anything
    int autoMask;      // 0/1
};

// The defaults every layer starts from, and the one a per-row "Reset" returns
// to. Identical to the historical single-layer defaults, so a fresh install
// and an old dlssnr_dshow.ini describe the same picture.
inline LayerCfg LayerDefault() {
    LayerCfg l;
    l.style        = 0;
    l.pctIntensity = 100;
    l.pctTone      = 100;
    l.pctStruct    = 100;
    l.pctSkin      = 0;
    l.autoMask     = 0;
    return l;
}

inline int LayerClampInt(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

// Defensive clamp. Every value that can reach the network goes through this:
// the ini is a text file a user can edit, and an out-of-range NGX parameter is
// not something the engine can be asked to survive.
inline LayerCfg LayerClamp(LayerCfg l) {
    l.style        = LayerClampInt(l.style, 0, 3);
    l.pctIntensity = LayerClampInt(l.pctIntensity, 0, 100);
    l.pctTone      = LayerClampInt(l.pctTone, 0, 200);
    l.pctStruct    = LayerClampInt(l.pctStruct, 0, 200);
    l.pctSkin      = LayerClampInt(l.pctSkin, 0, 200);
    l.autoMask     = l.autoMask ? 1 : 0;
    return l;
}

inline int LayerClampCount(int n) { return LayerClampInt(n, 1, (int)kMaxLayers); }

// ini key for parameter `base` of layer `layer` (1-based), written into `out`.
// Layer 1 keeps the HISTORICAL unprefixed keys ("style", "intensity", ...) so
// every existing dlssnr_dshow.ini keeps meaning exactly what it meant before
// layers existed; layers 2..8 are "layer2_style", "layer3_intensity", ...
//
// This is the single definition of that naming rule: the panel (writer) and
// ReadConfig (reader) both call it, which is what stops the two from drifting
// apart -- a mismatch there would show up as "the panel writes a setting that
// the next launch ignores", which is exactly the class of bug that is hard to
// see in a diff.
inline void LayerKeyName(int layer, const wchar_t* base, wchar_t* out, int cch) {
    if (!out || cch <= 0) return;
    if (layer <= 1) { _snwprintf_s(out, (size_t)cch, _TRUNCATE, L"%s", base); return; }
    const int n = LayerClampCount(layer);
    _snwprintf_s(out, (size_t)cch, _TRUNCATE, L"layer%d_%s", n, base);
}

} // namespace dlssnr
