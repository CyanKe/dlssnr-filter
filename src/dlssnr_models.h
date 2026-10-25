// ===========================================================================
//  dlssnr_models.h -- WHICH network inside nvngx_dlssnr.dll to run.
//
//  DLSSNR picks its network through a feature-CREATION parameter: the string
//  parameter "DLSSNR.Hint.Render.Preset" (the engine sets it in
//  EnsureNgxParams()). NVIDIA does not document the values, so the mapping below
//  is the one that was MEASURED against the two DLLs this project has:
//
//    * 310.8.0  carries ONE network, resource "WEIGHTS_HT".
//    * 310.9.16 carries TWO: "WEIGHTS_HT" (the 310.8.0 network, byte-identical)
//      and "WEIGHTS_CC_SMALL_FP8" (the lighter October model: 31 blocks / 76
//      layers instead of 71 / 152, ~22% fewer parameters, ~40% less network GPU
//      time -- that is the "2x faster model" NVIDIA announced for 2026-10-16).
//      Preset 1 selects HT, preset 2 selects CC_SMALL_FP8, and 0 (or no hint at
//      all) is the DLL's own default: on 310.9.16 that default IS CC_SMALL_FP8,
//      on 310.8.0 it is the only network there is.
//
//  Everything here is keyed on the RESOURCE NAME, not on the file version,
//  because the resource is what the DLL actually contains. A future 310.10 that
//  adds a third network shows up in the engine's DLL scan (dlssnr::DllWeights())
//  and can be selected from the panel's raw-preset button the day it ships,
//  before this table knows its name; adding a row here is what upgrades that
//  from "try preset N" to a labelled choice, and it is the only change needed --
//  the panel builds its buttons from this table, so nothing else moves.
//
//  Why one home for this: exactly the reason dlssnr_layers.h exists. Two copies
//  of "what does preset N mean" is how the panel ends up writing a setting the
//  engine reads differently, and that class of bug is invisible in a diff.
// ===========================================================================
#pragma once

#include <wchar.h>

namespace dlssnr {

// Presets are a small enumeration. The ceiling only exists so a hand-edited ini
// cannot ask for an absurd id; the panel's raw-preset button cycles up to it.
enum { kMaxModelPreset = 8 };

struct ModelInfo {
    int            preset;    // value written into DLSSNR.Hint.Render.Preset
    const char*    resource;  // the WEIGHTS_* name this preset needs; "" = the
                              // DLL's own default, which needs no resource
    const wchar_t* zh;        // panel label (short: it sits in a segment button)
    const wchar_t* en;
};

// The table. Sentinel-terminated (zh == nullptr) so ModelKnownCount() cannot
// drift from the rows: adding a row is the whole change.
inline const ModelInfo* ModelTable() {
    static const ModelInfo t[] = {
        { 0, "",                     L"自动", L"Auto" },
        { 1, "WEIGHTS_HT",           L"HT",   L"HT"   },
        { 2, "WEIGHTS_CC_SMALL_FP8", L"CC-S", L"CC-S" },
        { 0, nullptr, nullptr, nullptr },
    };
    return t;
}

inline int ModelKnownCount() {
    const ModelInfo* t = ModelTable();
    int n = 0;
    while (t[n].zh) ++n;
    return n;
}

// The table row for `preset`, or nullptr when this build does not know it (a
// preset a future DLL adds, reachable from the panel's raw-preset button).
inline const ModelInfo* ModelFind(int preset) {
    const ModelInfo* t = ModelTable();
    for (int i = 0; t[i].zh; ++i)
        if (t[i].preset == preset) return &t[i];
    return nullptr;
}

inline int ModelClamp(int preset) {
    if (preset < 0) return 0;
    if (preset > (int)kMaxModelPreset) return (int)kMaxModelPreset;
    return preset;
}

// A label for `preset` in the given UI language (0 = zh, 1 = en). Unknown
// presets get a number, because that is the only honest name for them: the DLL
// holds a network this build cannot identify, and the number is what the user
// typed / the button cycled to.
inline const wchar_t* ModelLabel(int preset, int lang, wchar_t* buf, int cch) {
    const ModelInfo* m = ModelFind(preset);
    if (m) return (lang == 1) ? m->en : m->zh;
    if (!buf || cch <= 0) return L"";
    _snwprintf_s(buf, (size_t)cch, _TRUNCATE, (lang == 1) ? L"#%d" : L"#%d", preset);
    return buf;
}

// The resource a preset needs, or "" when the DLL's default is what we want.
inline const char* ModelResource(int preset) {
    const ModelInfo* m = ModelFind(preset);
    return m ? m->resource : "";
}

} // namespace dlssnr
