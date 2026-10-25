// ===========================================================================
//  dlssnr_ui.h -- native settings window + notification-area icon
//
//  Replaces the Python control panel with the same shape LAV Filters and madVR
//  use: a tray icon for quick access, plus a real Win32 settings window with
//  real TRACKBARs. A tray *menu* cannot host a slider -- a Win32 menu is an
//  HMENU and holds only text/bitmap items, never a live control -- so the
//  sliders live in this window, exactly like madVR's settings dialog.
//
//  The one settings window serves two purposes:
//    * standalone, opened from the tray icon's "Settings..." item;
//    * embedded as the UI of the DirectShow property page, so the player's
//      Filters -> Properties dialog shows it as well.
//
//  Every control is created programmatically (no .rc, no .ico) so the
//  repository stays free of binary assets; the tray icon is drawn at runtime.
//
//  Depends on Engine (defined above) and on g_dir / g_hModule / kIniName.
// ===========================================================================
#pragma once

#include <commctrl.h>
#include <shellapi.h>
// Self-sufficient on purpose (the same reason dlssnr_layers.h is): the model row
// compares the DLL's resource names, and this header must not depend on the
// including translation unit having pulled <string.h> in first.
#include <string.h>

// ---------------------------------------------------------------------------
// option plumbing
// ---------------------------------------------------------------------------
static void DlssNrIniPath(wchar_t* buf, int cch) {
    _snwprintf_s(buf, cch, _TRUNCATE, L"%s\\%s", g_dir, kIniName);
}

static void DlssNrIniWriteInt(const wchar_t* key, int v) {
    wchar_t ini[MAX_PATH];
    DlssNrIniPath(ini, MAX_PATH);
    wchar_t s[32];
    _snwprintf_s(s, _countof(s), _TRUNCATE, L"%d", v);
    WritePrivateProfileStringW(L"DLSSNR", key, s, ini);
}

static bool DlssNrAutoshow();   // defined with the tray section below

static const wchar_t* kDlssNrStyleNames[4] = { L"默认", L"自然", L"电影", L"风格3" };
static const int kDlssNrStyleCount = 4;

// The model row's segments. Four, always:
//   0 = 自动 / Auto      -> preset 0, the DLL's own default (always available)
//   1 = HT               -> preset 1, needs the DLL to carry WEIGHTS_HT
//   2 = CC-S             -> preset 2, needs WEIGHTS_CC_SMALL_FP8
//   3 = 预设 N / Preset N -> the raw id escape hatch: it cycles the ids the
//                            three above do not cover, which is how a network a
//                            future DLL adds is selectable the day it ships.
// A segment whose resource the loaded DLL does not carry is drawn inert (not
// hidden) so the row keeps its shape and the absence is visible; the status
// page's model line names what the DLL actually has.
static const int kDlssNrModelBtnCount = 4;
static const int kDlssNrModelBtnCustom = 3;   // index of the raw-id segment
static const wchar_t* kDlssNrModelLblZh = L"模型";
static const wchar_t* kDlssNrModelLblEn = L"Model";
// Sliders are percentages of the engine's own units: 100% = 1.0, 200% = 2.0.
// 100% stays the reset default, so existing configurations and the old feel are
// unchanged (skin structure defaults to 0 = off, like Magpie's).
//
// Ranges are per parameter, because they were measured rather than assumed:
//   * NR intensity: DLSSNR.Intensity saturates at 1.0 -- 1.0 vs 2.0 is
//     bit-identical on real frames, so the slider stops at 100%.
//   * local tone / local structure: 2.0 really does keep changing the picture,
//     so those run to 200%.
static const int kDlssNrSliderCount = 4;
static const int kDlssNrSliderMax[kDlssNrSliderCount] = { 100, 200, 200, 200 };

// The per-row Reset target: LayerDefault()'s value for that row. The defaults
// (and the ranges above) live in exactly one place -- dlssnr_layers.h -- so a
// Reset cannot drift from the value the ini reader falls back to.
static int DlssNrSliderDefault(int i) {
    const dlssnr::LayerCfg d = dlssnr::LayerDefault();
    switch (i) {
    case 0:  return d.pctIntensity;
    case 1:  return d.pctTone;
    case 2:  return d.pctStruct;
    default: return d.pctSkin;
    }
}

// Tray-menu presets, one list per slider range.
static const int kDlssNrPct100[5] = { 0, 25, 50, 75, 100 };
static const int kDlssNrPct200[5] = { 0, 50, 100, 150, 200 };
static const int kDlssNrPctCount = 5;

static int DlssNrPctMax(float v, int maxPct) {
    int p = (int)(v * 100.0f + 0.5f);
    return p < 0 ? 0 : (p > maxPct ? maxPct : p);
}

// The single place that mutates the live engine and, optionally, the ini.
// The edit lands on the layer the panel is currently editing (Engine's selected
// layer). Layer 1 writes the historical unprefixed keys and layers 2..8 write
// layer<N>_*; dlssnr::LayerKeyName() owns that rule, so this writer cannot drift
// from ReadConfig() in dlssnr_dshow.cpp -- a drift there shows up as "the panel
// writes a setting the next launch ignores", which is invisible in a diff.
static void DlssNrApply(bool enabled, int style, int pctInt, int pctTone, int pctStruct,
                        int pctSkin, bool autoMask, bool persist) {
    Engine& e = Engine::Instance();
    e.SetOptions(enabled, style,
                 pctInt / 100.0f, pctTone / 100.0f, pctStruct / 100.0f,
                 pctSkin / 100.0f, autoMask);
    // One line per committed edit. Without it a field log cannot tell "the user
    // never changed anything" from "the change never reached the engine" -- which
    // is exactly the difference between a broken refresh and a disabled engine.
    if (persist)
        LogRaw("ui: applied layers=%d sel=%d enabled=%d style=%d intensity=%d%% "
               "tone=%d%% struct=%d%% skin=%d%% mask=%d",
               e.OptLayerCount(), e.OptSelLayer() + 1,
               enabled ? 1 : 0, style, pctInt, pctTone, pctStruct, pctSkin, autoMask ? 1 : 0);
    // A paused graph has no frames coming, so tell the filter(s) to re-render the
    // frame they cached. Push, not poll: this is the edit that just happened.
    DlssNrRefreshSinks(persist);
    if (!persist) return;
    DlssNrIniWriteInt(L"enabled", enabled ? 1 : 0);
    // The count travels with every commit too. The stepper/tray write it when the
    // chain's shape changes; writing it here as well keeps a stale ini from ever
    // describing a different chain than the one the engine is running.
    DlssNrIniWriteInt(L"layers", e.OptLayerCount());
    const int layer1 = e.OptSelLayer() + 1;
    wchar_t k[64];
    dlssnr::LayerKeyName(layer1, L"style",         k, _countof(k)); DlssNrIniWriteInt(k, style);
    dlssnr::LayerKeyName(layer1, L"intensity",     k, _countof(k)); DlssNrIniWriteInt(k, pctInt);
    dlssnr::LayerKeyName(layer1, L"localtone",     k, _countof(k)); DlssNrIniWriteInt(k, pctTone);
    dlssnr::LayerKeyName(layer1, L"localstruct",   k, _countof(k)); DlssNrIniWriteInt(k, pctStruct);
    dlssnr::LayerKeyName(layer1, L"skinstructure", k, _countof(k)); DlssNrIniWriteInt(k, pctSkin);
    dlssnr::LayerKeyName(layer1, L"automask",      k, _countof(k)); DlssNrIniWriteInt(k, autoMask ? 1 : 0);
}

// Persist the whole active chain: the count plus every layer's six keys. Used
// when the chain's SHAPE changes (the stepper, the tray's Layers menu, reset
// defaults), because there is no single "current layer" edit to write there.
// Growing copies the last layer into the new slots inside the engine, so without
// this the copy would not survive a restart even though it is what the user sees.
// Every key goes through LayerKeyName(), same as DlssNrApply() above.
static void DlssNrWriteLayers() {
    Engine& e = Engine::Instance();
    const int n = e.OptLayerCount();
    DlssNrIniWriteInt(L"layers", n);
    wchar_t k[64];
    for (int i = 0; i < n; ++i) {
        const dlssnr::LayerCfg l = e.OptLayer(i);
        const int layer1 = i + 1;
        dlssnr::LayerKeyName(layer1, L"style",         k, _countof(k)); DlssNrIniWriteInt(k, l.style);
        dlssnr::LayerKeyName(layer1, L"intensity",     k, _countof(k)); DlssNrIniWriteInt(k, l.pctIntensity);
        dlssnr::LayerKeyName(layer1, L"localtone",     k, _countof(k)); DlssNrIniWriteInt(k, l.pctTone);
        dlssnr::LayerKeyName(layer1, L"localstruct",   k, _countof(k)); DlssNrIniWriteInt(k, l.pctStruct);
        dlssnr::LayerKeyName(layer1, L"skinstructure", k, _countof(k)); DlssNrIniWriteInt(k, l.pctSkin);
        dlssnr::LayerKeyName(layer1, L"automask",      k, _countof(k)); DlssNrIniWriteInt(k, l.autoMask);
    }
}

// Extrapolation gain. Deliberately its own row and its own code path: this is
// NOT a layer count. A layer runs the network again; this re-uses the one pass's
// correction and scales it, which costs one extra texture read instead of a
// whole inference. The engine clamps to [100,400]; 100 IS off, and off is
// bit-identical to the build that shipped before this row existed.
static void DlssNrApplyExtrap(int pct, bool persist) {
    if (pct < 100) pct = 100;
    if (pct > 400) pct = 400;
    Engine::Instance().SetExtrap(pct);
    if (persist) DlssNrIniWriteInt(L"extrap", pct);
    LogRaw("ui: extrap = %d%%", pct);
    DlssNrRefreshSinks(persist);
}

// The late-frame switch. Deliberately NOT part of DlssNrApply(): it is not an
// NGX parameter, so it never reaches the network -- it only tells the filter's
// frame policy whether it may refuse frames that can no longer be shown on time
// (see ShouldDropLateFrame in dlssnr_dshow.cpp). Persistence and logging match
// the parameter path so it behaves like every other setting.
static void DlssNrApplyDrop(bool on, bool persist) {
    Engine::Instance().SetDropFrames(on);
    if (persist) DlssNrIniWriteInt(L"dropframes", on ? 1 : 0);
    LogRaw("ui: dropframes = %d", on ? 1 : 0);
}

// The reduced-resolution switch and its 25..100% slider. Like the drop switch it
// is not an NGX parameter -- it changes the SIZE of the engine's NGX feature, so
// applying it re-creates the session (the model stays loaded; ~0.2 s, during
// which frames pass through). That is also why the slider only applies on
// TB_ENDTRACK: rebuilding on every tick of a drag would strobe the picture.
static void DlssNrApplyProcScale(bool enabled, int pct, bool persist) {
    if (pct < 25)  pct = 25;
    if (pct > 100) pct = 100;
    Engine::Instance().SetProcScale(enabled, pct);
    if (persist) {
        DlssNrIniWriteInt(L"procenable", enabled ? 1 : 0);
        DlssNrIniWriteInt(L"procscale",  pct);
    }
    LogRaw("ui: procscale = %d%% (enabled=%d)", pct, enabled ? 1 : 0);
}

// ---------------------------------------------------------------------------
// the model row: which network inside nvngx_dlssnr.dll to run
// ---------------------------------------------------------------------------
// The DLL's own contents decide which segments can do anything, so this asks the
// engine's capability scan (dlssnr::DllWeights -- the WEIGHTS_* names in the
// loaded image) rather than trusting the file version. `preset` -1 in the
// returned list means "a network this build has no name for", which is exactly
// what the raw-id segment is for.
//
// A resource is "available" when the DLL carries the name the table maps to that
// preset. Preset 0 needs no resource (it IS the default), so it is always there.
static bool DlssNrModelAvailable(int preset, const dlssnr::DllWeight* w, int n) {
    if (preset == 0) return true;
    const char* need = dlssnr::ModelResource(preset);
    if (!need || !need[0]) return true;          // nothing named -> nothing to check
    for (int i = 0; i < n; ++i)
        if (strcmp(w[i].name, need) == 0) return true;
    return false;
}

// The preset a segment selects. Segments 0..2 ARE the preset ids; the last one is
// the raw id, whose value is whatever the user cycled it to.
static int DlssNrModelSegmentPreset(int seg, int rawId) {
    if (seg == kDlssNrModelBtnCustom) return dlssnr::ModelClamp(rawId);
    return seg;                                  // 0, 1, 2 == the preset ids
}

// The id the raw-id segment applies when clicked: cycle the ids the fixed
// segments do not cover, and jump to the first of them when a fixed segment is
// selected, so one click always lands somewhere new.
static int DlssNrModelCustomNext(int cur) {
    if (cur < kDlssNrModelBtnCustom) return kDlssNrModelBtnCustom;
    int id = cur + 1;
    if (id > (int)dlssnr::kMaxModelPreset) id = kDlssNrModelBtnCustom;
    return id;
}

// Which segment the current preset lights up. The raw-id segment owns every id
// the fixed segments do not name.
static bool DlssNrModelSegActive(int seg, int cur) {
    if (seg == kDlssNrModelBtnCustom) return cur >= kDlssNrModelBtnCustom;
    return cur == seg;
}

// Applying it is a session rebuild (the preset is a FEATURE-CREATION parameter,
// so the features have to be re-made), which is why this mirrors
// DlssNrApplyProcScale and not DlssNrApply: the worker rebuilds, frames pass
// through for the moment that takes, and a refused preset falls back inside the
// engine instead of costing the picture.
static void DlssNrApplyModel(int preset, bool persist) {
    preset = dlssnr::ModelClamp(preset);
    Engine::Instance().SetModel(preset);
    if (persist) DlssNrIniWriteInt(L"model", preset);
    // One line, and it names the resource the preset selects: "which network did
    // that switch to" is the whole question this feature exists to answer, and a
    // number alone does not answer it in a field log.
    const char* res = dlssnr::ModelResource(preset);
    LogRaw("ui: model preset = %d (%s)", preset,
           (res && res[0]) ? res : "DLL default");
}

// ---------------------------------------------------------------------------
// settings window
// ---------------------------------------------------------------------------
// ---------------------------------------------------------------------------
// UI language (ini key: lang = zh | en)
// ---------------------------------------------------------------------------
static int g_uiLang = -1;          // -1 = not read yet, 0 = 中文, 1 = English

static void DlssNrLoadLang() {
    wchar_t ini[MAX_PATH];
    DlssNrIniPath(ini, MAX_PATH);
    wchar_t buf[16] = L"";
    GetPrivateProfileStringW(L"DLSSNR", L"lang", L"zh", buf, _countof(buf), ini);
    g_uiLang = (_wcsicmp(buf, L"en") == 0) ? 1 : 0;
}

static const wchar_t* T(const wchar_t* zh, const wchar_t* en) {
    if (g_uiLang < 0) DlssNrLoadLang();
    return (g_uiLang == 1) ? en : zh;
}

// Format name from the nominal bytes-per-pixel value the filter publishes
// (1 = planar YUV 4:2:0, 3 = BGR24, 4 = BGR32). The engine speaks NV12 end to
// end since the GPU rewrite -- the old "(converted to BGR24)" wording survived
// it and only confused the status page.
static const wchar_t* DlssNrFmtName(int bpp) {
    if (bpp == 1) return T(L"NV12（引擎原生格式）", L"NV12 (engine format)");
    if (bpp == 3) return T(L"RGB24（引擎不处理，直通）", L"RGB24 (passthrough)");
    if (bpp == 4) return T(L"RGB32（引擎不处理，直通）", L"RGB32 (passthrough)");
    return T(L"未知", L"unknown");
}

#define DLS_N_CHANGED   (WM_APP + 10)   // posted to the parent on any edit

// ---------------------------------------------------------------------------
// dark theme
//
// The panel is custom drawn. Win32's themed controls cannot be recoloured
// reliably -- a combobox even owns a separate list window -- so instead:
//   * buttons/tabs/style segments are BS_OWNERDRAW and painted in WM_DRAWITEM,
//   * the checkboxes stay real BS_AUTOCHECKBOX controls and paint themselves
//     through a subclass (the owner-draw style would drop their state),
//   * the trackbars are subclassed and paint themselves,
//   * statics get a dark brush and a light text colour via WM_CTLCOLORSTATIC.
// Only colours and geometry live here, so the palette is easy to retune.
// ---------------------------------------------------------------------------
static const COLORREF kClrBg       = RGB(0x16, 0x16, 0x16);   // page background
static const COLORREF kClrStrip    = RGB(0x24, 0x24, 0x24);   // tab strip
static const COLORREF kClrText     = RGB(0xEA, 0xEA, 0xEA);
static const COLORREF kClrTextDim  = RGB(0x96, 0x96, 0x96);
static const COLORREF kClrAccent   = RGB(0xB4, 0x18, 0x1A);   // red accent
static const COLORREF kClrAccentLt = RGB(0xE6, 0x3C, 0x3C);
static const COLORREF kClrGroove   = RGB(0x46, 0x46, 0x46);   // slider rail
static const COLORREF kClrFace     = RGB(0x30, 0x30, 0x30);   // button face
static const COLORREF kClrFaceHot  = RGB(0x45, 0x45, 0x45);
static const COLORREF kClrEdge     = RGB(0x5A, 0x5A, 0x5A);
static const COLORREF kClrThumb    = RGB(0xCC, 0xCC, 0xCC);
static const COLORREF kClrOk       = RGB(0x4A, 0xCC, 0x4A);
static const COLORREF kClrWarn     = RGB(0xE6, 0xA8, 0x38);
static const COLORREF kClrErr      = RGB(0xE6, 0x54, 0x54);

// Solid brushes are cached: the panel repaints on every 500 ms telemetry tick.
static HBRUSH DlssNrBrush(COLORREF c) {
    static COLORREF keys[10] = {};
    static HBRUSH   brushes[10] = {};
    static int      count = 0;
    for (int i = 0; i < count; ++i) if (keys[i] == c) return brushes[i];
    if (count < (int)_countof(keys)) {
        HBRUSH b = CreateSolidBrush(c);
        keys[count] = c; brushes[count] = b; ++count;
        return b;
    }
    return (HBRUSH)GetStockObject(BLACK_BRUSH);
}

static void DlssNrFill(HDC dc, const RECT& rc, COLORREF c) {
    FillRect(dc, &rc, DlssNrBrush(c));
}

static void DlssNrFrame(HDC dc, const RECT& rc, COLORREF c) {
    FrameRect(dc, &rc, DlssNrBrush(c));
}

enum {
    // tab strip
    IDC_DLS_TAB0    = 1001,   // +0 parameters, +1 status
    IDC_DLS_TAB2    = 1006,   // the compare page
    // language segments
    IDC_DLS_LANG0   = 1004,   // +0 中文, +1 English
    // style segments
    IDC_DLS_STYLE0  = 1010,   // +0..3
    // one row per slider: label / trackbar / value / reset
    IDC_DLS_LBL0    = 1020,   // + i * 10
    IDC_DLS_TRK0    = 1021,
    IDC_DLS_VAL0    = 1022,
    IDC_DLS_RST0    = 1023,
    // checkboxes
    IDC_DLS_ENABLE  = 1100,
    IDC_DLS_AUTOMASK= 1101,
    IDC_DLS_AUTOSHOW= 1102,
    IDC_DLS_DROP    = 1103,
    IDC_DLS_PROCCHK = 1104,
    // The reduced-resolution slider is NOT one of the kDlssNrSliderCount rows:
    // its range is 25..100 and it re-creates the engine session instead of
    // pushing an NGX parameter, so it needs its own plumbing.
    IDC_DLS_PROCLBL = 1130,
    IDC_DLS_PROCTRK = 1131,
    IDC_DLS_PROCVAL = 1132,
    // The extrapolation row: same shape as the processing-resolution row above,
    // and also NOT one of the kDlssNrSliderCount rows -- its range is 100..400
    // because it is a GAIN, not a strength, and 100 means off.
    IDC_DLS_EXTRAPLBL = 1135,
    IDC_DLS_EXTRAPTRK = 1136,
    IDC_DLS_EXTRAPVAL = 1137,
    // Both global rows get a Reset in the same column the per-layer rows use.
    // They used to have none, on the argument that "100 % IS the reset" -- true,
    // and still not what a user reaching for the button next to the slider
    // expects. Reset here means the documented default, not "off": for the
    // processing resolution that is 50 % (the value it restores to), for
    // extrapolation it is 100 % (= off, which is the documented default).
    IDC_DLS_PROCRST   = 1133,
    IDC_DLS_EXTRAPRST = 1138,
    // Layer count stepper and the current-layer strip. The strip is
    // IDC_DLS_LAYER0 + i for layer i (0-based), so hit-testing the whole strip is
    // one range check -- the same shape the style segments already use.
    IDC_DLS_LAYERNAME   = 1140,   // "层数 / Layers" caption
    IDC_DLS_LCOUNT_MINUS= 1141,   // the - half of the stepper
    IDC_DLS_LAYERCOUNT  = 1142,   // the N readout between them
    IDC_DLS_LCOUNT_PLUS = 1143,   // the + half
    IDC_DLS_LAYERNOTE   = 1144,   // per-layer cost note under the row
    IDC_DLS_LAYER0      = 1150,   // +0..kMaxLayers-1: select the edited layer
    // actions
    IDC_DLS_RESETALL= 1110,
    IDC_DLS_RECHECK = 1111,
    IDC_DLS_LOG     = 1112,
    // borderless window header (standalone panel only)
    IDC_DLS_CLOSE   = 1003,
    // status page
    IDC_DLS_TITLE   = 1120,
    IDC_DLS_SUB     = 1121,
    IDC_DLS_STATS   = 1122,
    IDC_DLS_GEOM    = 1123,
    IDC_DLS_HINT    = 1124,
    IDC_DLS_LAYERS  = 1125,   // configured vs actually-built layer counts
    // The model row: one segment per choice, plus a caption and a status line.
    // Which network inside nvngx_dlssnr.dll to run -- see src\dlssnr_models.h.
    IDC_DLS_MODELLBL = 1160,  // "模型 / Model" caption
    IDC_DLS_MODEL0   = 1161,  // +0..kDlssNrModelBtnCount-1
    IDC_DLS_MODELLINE= 1170,  // status page: which network is in effect
};

struct DlssNrSettings {
    HWND hwnd;
    HWND btnClose;                   // borderless header (standalone only)
    HWND tab[3], lang[2], styleBtn[kDlssNrStyleCount];
    HWND lbl[4], trk[4], val[4], rst[4];
    HWND chkEnable, chkAutoMask, chkDrop, chkProc, chkAutoshow;
    HWND lblProc, trkProc, valProc;  // the "processing resolution" row (25..100%)
    HWND lblExtrap, trkExtrap, valExtrap;   // extrapolation gain row (100..400%, 100 = off)
    HWND rstProc, rstExtrap;                // per-row Reset for the two global rows
    // The model row: which network inside nvngx_dlssnr.dll to run. Session-level
    // like the processing-resolution row above it, and built from
    // src\dlssnr_models.h so a new network is one table row, not new plumbing.
    HWND lblModel, modelBtn[kDlssNrModelBtnCount];
    HWND lblModelLine;               // status page: which network is in effect
    // Layer count stepper + current-layer strip. The strip selects WHICH layer
    // the style / sliders / auto-mask edit, so it sits above all of them.
    HWND lblLayerName, lblLayerCount, lblLayerNote;
    HWND btnLayerMinus, btnLayerPlus;
    HWND layerBtn[dlssnr::kMaxLayers];   // the 1..N strip (created once, shown per count)
    HWND btnResetAll, btnRecheck, btnLog;
    HWND lblTitle, lblSub, lblStats, lblGeom, lblHint, lblLayers;
    HFONT font, fontBold, fontBig, fontSmall;
    int   page;                      // 0 = parameters, 1 = status, 2 = compare
    int   shownStyle;                // style the segments currently show
    int   shownLayerCount;           // N the stepper/strip currently show
    int   shownSelLayer;             // layer the strip currently highlights
    int   shownModel;                // preset the model segments currently show
    HWND  compareWnd;                // the 对比 window (created on first use)
    int   compareSplit;              // divider position, percent of the width
    bool  standalone;
    void (*onChanged)(void*);
    void* onChangedCtx;
};

// ---- geometry --------------------------------------------------------------
//
// Compact dark layout, measured from the real dialog font and scaled by it.
// The panel runs inside the player process, which is normally DPI aware, so a
// hardcoded pixel layout truncates the moment the font is bigger than the one
// it was tuned on. Both languages are measured and the wider result is used, so
// switching language never resizes the window.
//
// Targets at 96 DPI: about 460 x 265 px -- one tab strip, one row per slider
// with its own Reset, then checkboxes and the actions.
struct DlssNrMetrics {
    int lineH;                     // one text line
    int rowH;                      // slider row height
    int rowPitch;                  // slider row pitch (compact)
    int segH;                      // small button / segment height
    int tabH;                      // tab strip height
    int margin, gap, btnGap;       // spacing
    int lblW, barW, valW, rstW;    // slider row columns
    int tabW, langW, styleW;       // segmented buttons
    int   headerH;                   // own title bar (borderless standalone window)
    int   closeW, closeH;            // header close button
    int   layerRowH;                 // count stepper + current-layer strip row
    int   layerStepW, layerCountW;   // stepper button / N readout widths
    int   layerBtnW;                 // one button of the 1..N strip
    int   layerNoteH;                // the dim per-layer cost note under the row
    int   modelBtnW;                 // one segment of the model row
    int   chkW[5];                   // checkbox widths (box + label)
    int resetAllW, recheckW, logW; // action buttons
    int boxW, thumbW, thumbH, grooveH;
    int titleH;                    // big status title
    int hintH;
    int pageH;                     // height of the taller tab page
    int wantW, wantH;
};

static int DlssNrExtent(HDC dc, const wchar_t* s) {
    SIZE sz = {};
    GetTextExtentPoint32W(dc, s, (int)wcslen(s), &sz);
    return sz.cx;
}

static int DlssNrMax2(int a, int b) { return a > b ? a : b; }

// The late-frame switch's label lives in one place: the layout measures both
// languages, the checkbox paints it, and the tray menu offers the same thing.
static const wchar_t* kDlssNrDropZh = L"丢帧保同步";
static const wchar_t* kDlssNrDropEn = L"Drop late frames";

// The reduced-resolution switch and its slider label.
static const wchar_t* kDlssNrProcChkZh = L"降分辨率处理";
static const wchar_t* kDlssNrProcChkEn = L"Reduce resolution";
static const wchar_t* kDlssNrProcLblZh = L"处理分辨率";
static const wchar_t* kDlssNrProcLblEn = L"Processing res.";
// "外推" is the name the measurements earned: extrapolation reproduces the look
// of a second pass, it does not perform one. Calling it "layers" would be a lie
// the panel could not keep -- the numbers diverge from layer 3 on.
static const wchar_t* kDlssNrExtrapLblZh = L"外推";
static const wchar_t* kDlssNrExtrapLblEn = L"Extrapolate";

// The layer row's caption and its cost note. One definition each: the layout
// measures both languages in DlssNrGetMetrics(), and Retext paints the one the
// current language needs -- a second copy is how a language switch ends up
// clipping a line the layout sized for the other language.
static const wchar_t* kDlssNrLayersLblZh = L"层数";
static const wchar_t* kDlssNrLayersLblEn = L"Layers";
static const wchar_t* kDlssNrLayerNoteZh =
    L"每层是一次完整推理；风格、滑块和自动遮罩都作用于「当前层」。"
    L"成本随层数线性增加，2–3 层是实用范围。";
static const wchar_t* kDlssNrLayerNoteEn =
    L"Each layer is a full pass; style, sliders and auto-mask all act on the "
    L"current layer. Cost grows with the count; 2-3 layers is practical.";

// The bottom hint. One definition, used by both the metrics (which measures it in
// both languages to size the page) and the control that paints it -- the two used
// to hold separate copies of the same sentence, which is how a language switch
// ends up clipping a line the layout sized for the other language.
static const wchar_t* kDlssNrHintZh =
    L"参数实时生效；取消勾选「启用 DLSSNR」为真正的直通。"
    L"引擎跟不上时，可勾「丢帧保同步」或「降分辨率处理」。"
    L"「模型」选择 DLL 里的哪个网络，切换会重建会话。";
static const wchar_t* kDlssNrHintEn =
    L"Live: uncheck Enable DLSSNR for a true pass-through. "
    L"If the engine cannot keep up, use Drop late frames or Reduce resolution. "
    L"Model picks which network inside the DLL to run; switching rebuilds the session.";

static const wchar_t* DlssNrStyleNameLang(int i, int lang) {
    static const wchar_t* zh[kDlssNrStyleCount] = { L"默认", L"自然", L"电影", L"风格3" };
    static const wchar_t* en[kDlssNrStyleCount] = { L"Default", L"Natural", L"Cinema", L"Style 3" };
    if (i < 0 || i >= kDlssNrStyleCount) i = 0;
    return (lang == 1) ? en[i] : zh[i];
}

static const wchar_t* DlssNrStyleName(int i) { return DlssNrStyleNameLang(i, g_uiLang); }

// Deliberately measured in both languages: the window must not resize (or wrap)
// just because someone switched the panel language.
static int DlssNrMaxExtentLang(HDC dc, const wchar_t* zh, const wchar_t* en) {
    return DlssNrMax2(DlssNrExtent(dc, zh), DlssNrExtent(dc, en));
}

static const DlssNrMetrics& DlssNrGetMetrics() {
    static DlssNrMetrics m;
    static bool ready = false;
    if (ready) return m;

    // Same font the window uses (see WM_CREATE), so the measurements match.
    NONCLIENTMETRICSW ncm = { sizeof(NONCLIENTMETRICSW) };
    HFONT font = nullptr;
    if (SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0)) {
        font = CreateFontIndirectW(&ncm.lfMessageFont);
    }
    HDC dc = CreateCompatibleDC(nullptr);
    HGDIOBJ oldFont = font ? SelectObject(dc, font) : nullptr;

    TEXTMETRICW tm = {};
    GetTextMetricsW(dc, &tm);
    m.lineH = tm.tmHeight > 0 ? tm.tmHeight : 16;

    // 16 px is the dialog font at 96 DPI; anything larger is DPI scaling and
    // every fixed pixel value has to follow it.
    float sc = m.lineH / 16.0f;
    if (sc < 1.0f) sc = 1.0f;
    if (sc > 3.0f) sc = 3.0f;
    auto S = [sc](int v) { return (int)(v * sc + 0.5f); };

    m.margin   = S(10);
    m.gap      = S(6);
    m.btnGap   = S(4);
    m.rowH     = m.lineH + S(6);
    m.rowPitch = m.rowH + S(8);
    m.segH     = S(22);
    m.tabH     = S(26);
    m.titleH   = S(30);
    m.boxW     = S(14);
    m.thumbW   = S(9);
    m.thumbH   = S(18);
    m.grooveH  = S(4);
    m.barW     = S(230);
    m.headerH  = S(30);
    m.closeW   = S(26);
    m.closeH   = S(22);

    // Layer strip: two small stepper halves around the N readout, then one digit
    // button per layer, up to the kMaxLayers ceiling. Everything is measured
    // rather than assumed, so a CJK font or a DPI scale cannot clip the digits.
    const int digitW = DlssNrExtent(dc, L"8");
    m.layerStepW  = DlssNrMax2(digitW, m.lineH) + S(12);
    m.layerCountW = digitW + S(10);
    m.layerBtnW   = digitW + S(14);
    m.layerRowH   = m.segH;

    m.lblW = DlssNrMaxExtentLang(dc, L"局部结构", L"Local structure");
    m.lblW = DlssNrMax2(m.lblW, DlssNrMaxExtentLang(dc, L"局部色调", L"Local tone"));
    m.lblW = DlssNrMax2(m.lblW, DlssNrMaxExtentLang(dc, L"皮肤结构", L"Skin structure"));
    m.lblW = DlssNrMax2(m.lblW, DlssNrMaxExtentLang(dc, kDlssNrProcLblZh, kDlssNrProcLblEn));
    m.lblW = DlssNrMax2(m.lblW, DlssNrMaxExtentLang(dc, kDlssNrExtrapLblZh, kDlssNrExtrapLblEn));
    m.lblW += S(8);

    m.valW = DlssNrExtent(dc, L"200%") + S(6);
    m.rstW = DlssNrMaxExtentLang(dc, L"重置", L"Reset") + S(18);
    m.tabW = DlssNrMax2(DlssNrMaxExtentLang(dc, L"参数", L"Parameters"),
                        DlssNrMaxExtentLang(dc, L"状态", L"Status")) + S(24);
    m.langW = DlssNrMax2(DlssNrExtent(dc, L"中文"), DlssNrExtent(dc, L"English")) + S(18);
    m.styleW = 0;
    for (int lang = 0; lang < 2; ++lang)
        for (int i = 0; i < kDlssNrStyleCount; ++i)
            m.styleW = DlssNrMax2(m.styleW, DlssNrExtent(dc, DlssNrStyleNameLang(i, lang)));
    m.styleW += S(16);

    // The model row's segments: measured over every label the table defines plus
    // the widest raw-id form, in both languages, so neither a CJK font nor a row
    // added to dlssnr_models.h later can clip a segment.
    m.modelBtnW = 0;
    for (int i = 0; i < dlssnr::ModelKnownCount(); ++i) {
        const dlssnr::ModelInfo& mi = dlssnr::ModelTable()[i];
        m.modelBtnW = DlssNrMax2(m.modelBtnW, DlssNrMaxExtentLang(dc, mi.zh, mi.en));
    }
    m.modelBtnW = DlssNrMax2(m.modelBtnW, DlssNrMaxExtentLang(dc, L"预设 8", L"Preset 8"));
    m.modelBtnW += S(16);

    m.chkW[0] = DlssNrMaxExtentLang(dc, L"启用 DLSSNR", L"Enable DLSSNR") + m.boxW + S(10);
    m.chkW[1] = DlssNrMaxExtentLang(dc, L"自动遮罩", L"Automatic mask") + m.boxW + S(10);
    m.chkW[2] = DlssNrMaxExtentLang(dc, kDlssNrDropZh, kDlssNrDropEn) + m.boxW + S(10);
    m.chkW[3] = DlssNrMaxExtentLang(dc, kDlssNrProcChkZh, kDlssNrProcChkEn) + m.boxW + S(10);
    m.chkW[4] = DlssNrMaxExtentLang(dc, L"启动时显示控制面板", L"Show panel on playback")
              + m.boxW + S(10);
    m.resetAllW = DlssNrMaxExtentLang(dc, L"重置为默认", L"Reset defaults") + S(18);
    m.recheckW  = DlssNrMaxExtentLang(dc, L"重新检测", L"Recheck") + S(18);
    m.logW      = DlssNrMaxExtentLang(dc, L"打开日志", L"Open log") + S(18);

    // Width: the slider row decides, but never clip a row that needs more.
    m.wantW = m.margin * 2 + m.lblW + m.gap + m.barW + m.gap + m.valW + m.gap + m.rstW;
    const int tabRowW = m.margin * 2 + 3 * m.tabW + 2 * m.btnGap + S(16) + 2 * m.langW + m.btnGap;
    if (tabRowW > m.wantW) m.wantW = tabRowW;
    const int styleRowW = m.margin * 2 + m.lblW + m.gap
        + kDlssNrStyleCount * m.styleW + (kDlssNrStyleCount - 1) * m.btnGap;
    if (styleRowW > m.wantW) m.wantW = styleRowW;
    // The strip is sized for the worst case (kMaxLayers buttons) even while only
    // one is shown: the window must not resize when the count goes up.
    const int layerRowW = m.margin * 2 + m.lblW + m.gap
        + m.layerStepW + m.layerCountW + m.layerStepW
        + m.gap + (int)dlssnr::kMaxLayers * m.layerBtnW
        + ((int)dlssnr::kMaxLayers - 1) * m.btnGap;
    if (layerRowW > m.wantW) m.wantW = layerRowW;
    // The model row is sized for ALL of its segments even when the loaded DLL
    // cannot serve some of them (those are drawn inert, not hidden): a different
    // nvngx_dlssnr.dll must not resize the panel.
    const int modelRowW = m.margin * 2 + m.lblW + m.gap
        + kDlssNrModelBtnCount * m.modelBtnW + (kDlssNrModelBtnCount - 1) * m.btnGap;
    if (modelRowW > m.wantW) m.wantW = modelRowW;
    // Two rows of checkboxes: (enable, mask, drop frames) then (resolution, autoshow).
    const int chkRow1W = m.margin * 2 + m.chkW[0] + m.chkW[1] + m.chkW[2] + 2 * m.gap;
    const int chkRow2W = m.margin * 2 + m.chkW[3] + m.chkW[4] + m.gap;
    if (chkRow1W > m.wantW) m.wantW = chkRow1W;
    if (chkRow2W > m.wantW) m.wantW = chkRow2W;
    const int actionRowW = m.margin * 2 + m.resetAllW + m.recheckW + m.logW + 4 * m.gap;
    if (actionRowW > m.wantW) m.wantW = actionRowW;

    // Hint: measured in both languages, wrapping inside the page width.
    const int hintW = m.wantW - 2 * m.margin;
    m.hintH = 0;
    {
        for (int lang = 0; lang < 2; ++lang) {
            RECT rc = { 0, 0, hintW, 0 };
            DrawTextW(dc, lang ? kDlssNrHintEn : kDlssNrHintZh, -1, &rc,
                      DT_CALCRECT | DT_WORDBREAK | DT_NOPREFIX);
            m.hintH = DlssNrMax2(m.hintH, rc.bottom - rc.top);
        }
        m.hintH += S(2);
    }

    // The per-layer note under the strip, wrapped inside the page width and
    // measured in both languages. It is where the UI says both "what the strip
    // scopes" and "N layers cost N full passes", so it must not be the line that
    // clips when the language changes.
    m.layerNoteH = 0;
    {
        for (int lang = 0; lang < 2; ++lang) {
            RECT rc = { 0, 0, hintW, 0 };
            DrawTextW(dc, lang ? kDlssNrLayerNoteEn : kDlssNrLayerNoteZh, -1, &rc,
                      DT_CALCRECT | DT_WORDBREAK | DT_NOPREFIX);
            m.layerNoteH = DlssNrMax2(m.layerNoteH, rc.bottom - rc.top);
        }
        m.layerNoteH += S(2);
    }

    const int paramsH = m.layerRowH + m.gap                             // count + current-layer strip
                      + m.layerNoteH + m.gap                            // per-layer cost note
                      + m.segH + m.gap                                  // style row
                      + (kDlssNrSliderCount + 2) * m.rowPitch           // sliders + processing res. + extrapolation
                      + m.segH + m.gap                                  // model row
                      + m.gap + 2 * m.rowH + m.gap / 2                  // two rows of checkboxes
                      + m.gap + m.segH                                  // actions
                      + m.gap + m.hintH;                                // hint
    const int statusH = m.titleH + m.gap + 5 * m.lineH + 2 * S(4);
    m.pageH = DlssNrMax2(paramsH, statusH);
    m.wantH = m.margin + m.tabH + m.gap + m.pageH + m.margin;

    if (oldFont) SelectObject(dc, oldFont);
    DeleteDC(dc);
    if (font) DeleteObject(font);

    ready = true;
    return m;
}

// The standalone panel is a borderless window with its own title bar, so it is
// exactly one header tall more than the same panel embedded in the player's
// property sheet.
static int DlssNrWantWidth(bool standalone)  { (void)standalone; return DlssNrGetMetrics().wantW; }
static int DlssNrWantHeight(bool standalone) {
    const DlssNrMetrics& m = DlssNrGetMetrics();
    return m.wantH + (standalone ? m.headerH : 0);
}

static bool DlssNrChecked(HWND h) {
    return h && SendMessageW(h, BM_GETCHECK, 0, 0) == BST_CHECKED;
}

static void DlssNrSetChecked(HWND h, bool on) {
    if (!h) return;
    if (DlssNrChecked(h) == on) return;
    SendMessageW(h, BM_SETCHECK, on ? BST_CHECKED : BST_UNCHECKED, 0);
    InvalidateRect(h, nullptr, FALSE);
}

// ---- custom drawing --------------------------------------------------------
static bool DlssNrIsHot(HWND h) { return GetWindowLongPtrW(h, GWLP_USERDATA) != 0; }

static void DlssNrSetHot(HWND h, bool hot) {
    if (DlssNrIsHot(h) == hot) return;
    SetWindowLongPtrW(h, GWLP_USERDATA, hot ? 1 : 0);
    InvalidateRect(h, nullptr, FALSE);
}

static void DlssNrSelectFont(HDC dc, HWND c, HGDIOBJ* old) {
    HFONT f = (HFONT)SendMessageW(c, WM_GETFONT, 0, 0);
    *old = f ? SelectObject(dc, f) : nullptr;
}

static void DlssNrDrawButton(const DRAWITEMSTRUCT* di, const wchar_t* label, bool active) {
    const RECT rc = di->rcItem;
    const bool pressed  = (di->itemState & ODS_SELECTED) != 0;
    const bool disabled = (di->itemState & ODS_DISABLED) != 0;
    const bool hot      = DlssNrIsHot(di->hwndItem);
    const bool isClose  = (di->CtlID == IDC_DLS_CLOSE);

    COLORREF bg = kClrFace;
    COLORREF edge = active ? kClrAccentLt : (hot ? kClrEdge : kClrStrip);
    if (isClose) {
        // Blend into the title bar, and turn red on hover (reference look).
        bg = pressed ? kClrAccent : (hot ? kClrFaceHot : kClrStrip);
        edge = hot ? kClrAccentLt : kClrStrip;
    } else if (disabled)     bg = kClrFace;
    else if (active)         bg = pressed ? kClrAccentLt : kClrAccent;
    else if (pressed)        bg = kClrAccent;
    else if (hot)            bg = kClrFaceHot;

    DlssNrFill(di->hDC, rc, bg);
    DlssNrFrame(di->hDC, rc, edge);

    SetBkMode(di->hDC, TRANSPARENT);
    SetTextColor(di->hDC, disabled ? kClrTextDim
                                    : (isClose ? (hot ? kClrText : kClrAccentLt)
                                               : kClrText));
    HGDIOBJ old = nullptr;
    DlssNrSelectFont(di->hDC, di->hwndItem, &old);
    RECT t = rc;
    DrawTextW(di->hDC, label, -1, &t, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    if (old) SelectObject(di->hDC, old);

    if ((di->itemState & ODS_FOCUS) && !disabled) {   // keyboard focus
        RECT f = rc;
        InflateRect(&f, -2, -2);
        DrawFocusRect(di->hDC, &f);
    }
}

static const wchar_t* DlssNrButtonLabel(int id);   // defined with the text helpers

// A checkbox keeps its real BS_AUTOCHECKBOX behaviour and paints itself through
// a subclass: BS_AUTOCHECKBOX | BS_OWNERDRAW would collapse to plain
// BS_OWNERDRAW (both share the low style nibble), which silently drops the
// check state and the auto-toggle.
static void DlssNrPaintCheckbox(HWND h, HDC dc, const wchar_t* label) {
    const DlssNrMetrics& m = DlssNrGetMetrics();
    RECT rc = {};
    GetClientRect(h, &rc);
    const bool checked = DlssNrChecked(h);
    const bool hot     = DlssNrIsHot(h);
    const bool focused = GetFocus() == h;

    DlssNrFill(dc, rc, kClrBg);

    RECT box = { rc.left + 1, rc.top + (rc.bottom - rc.top - m.boxW) / 2,
                 rc.left + 1 + m.boxW, 0 };
    box.bottom = box.top + m.boxW;
    DlssNrFill(dc, box, checked ? kClrAccent : (hot ? kClrFaceHot : kClrFace));
    DlssNrFrame(dc, box, checked ? kClrAccentLt : kClrEdge);

    if (checked) {                    // tick
        HPEN pen = CreatePen(PS_SOLID, DlssNrMax2(1, m.boxW / 7), kClrText);
        HGDIOBJ oldPen = SelectObject(dc, pen);
        MoveToEx(dc, box.left + m.boxW * 2 / 10, box.top + m.boxW * 5 / 10, nullptr);
        LineTo(dc,   box.left + m.boxW * 4 / 10, box.top + m.boxW * 7 / 10);
        LineTo(dc,   box.left + m.boxW * 8 / 10, box.top + m.boxW * 3 / 10);
        SelectObject(dc, oldPen);
        DeleteObject(pen);
    }

    RECT t = rc;
    t.left = box.right + m.gap;
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, kClrText);
    HGDIOBJ old = nullptr;
    DlssNrSelectFont(dc, h, &old);
    DrawTextW(dc, label, -1, &t, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    if (old) SelectObject(dc, old);

    if (focused) {
        RECT f = box;
        InflateRect(&f, 2, 2);
        DrawFocusRect(dc, &f);
    }
}

static LRESULT CALLBACK DlssNrCheckProc(HWND h, UINT msg, WPARAM wp, LPARAM lp,
                                        UINT_PTR, DWORD_PTR) {
    switch (msg) {
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps = {};
        HDC dc = BeginPaint(h, &ps);
        DlssNrPaintCheckbox(h, dc, DlssNrButtonLabel(GetDlgCtrlID(h)));
        EndPaint(h, &ps);
        return 0;
    }
    case WM_SETFOCUS:
    case WM_KILLFOCUS:
        InvalidateRect(h, nullptr, FALSE);
        break;
    case WM_MOUSEMOVE: {
        DlssNrSetHot(h, true);
        TRACKMOUSEEVENT tme = { sizeof(tme), TME_LEAVE, h, 0 };
        TrackMouseEvent(&tme);
        break;
    }
    case WM_MOUSELEAVE:
        DlssNrSetHot(h, false);
        break;
    }
    return DefSubclassProc(h, msg, wp, lp);
}

// The trackbars are subclassed and painted here: a themed trackbar cannot be
// recoloured, and TBS_OWNERDRAW only hands over the whole control anyway.
static void DlssNrDrawSlider(HWND trk, HDC dc) {
    const DlssNrMetrics& m = DlssNrGetMetrics();
    RECT rc = {};
    GetClientRect(trk, &rc);
    DlssNrFill(dc, rc, kClrBg);
    if (rc.right <= rc.left + m.thumbW) return;

    const int pos  = (int)SendMessageW(trk, TBM_GETPOS, 0, 0);
    const int lo   = (int)SendMessageW(trk, TBM_GETRANGEMIN, 0, 0);
    const int hi   = (int)SendMessageW(trk, TBM_GETRANGEMAX, 0, 0);
    const int span = hi > lo ? hi - lo : 1;
    const int travel = rc.right - rc.left - m.thumbW;
    int thumbX = rc.left + m.thumbW / 2
               + (int)((long long)(pos - lo) * travel / span);

    const int cy = (rc.top + rc.bottom) / 2;
    RECT rail = { rc.left, cy - m.grooveH / 2, rc.right, cy + m.grooveH / 2 };
    DlssNrFill(dc, rail, kClrGroove);
    RECT filled = rail;
    filled.right = thumbX;
    DlssNrFill(dc, filled, kClrAccent);

    RECT thumb = { thumbX - m.thumbW / 2, cy - m.thumbH / 2,
                   thumbX + m.thumbW / 2, cy + m.thumbH / 2 };
    DlssNrFill(dc, thumb, DlssNrIsHot(trk) ? kClrText : kClrThumb);
    DlssNrFrame(dc, thumb, kClrBg);

    if (GetFocus() == trk) {
        RECT f = rc;
        InflateRect(&f, -1, -1);
        DrawFocusRect(dc, &f);
    }
}

static LRESULT CALLBACK DlssNrSliderProc(HWND h, UINT msg, WPARAM wp, LPARAM lp,
                                         UINT_PTR, DWORD_PTR) {
    switch (msg) {
    case WM_ERASEBKGND:
        return 1;                                  // WM_PAINT fills everything
    case WM_PAINT: {
        PAINTSTRUCT ps = {};
        HDC dc = BeginPaint(h, &ps);
        DlssNrDrawSlider(h, dc);
        EndPaint(h, &ps);
        return 0;
    }
    case WM_SETFOCUS:
    case WM_KILLFOCUS:
        InvalidateRect(h, nullptr, FALSE);
        break;
    }
    return DefSubclassProc(h, msg, wp, lp);
}

static LRESULT CALLBACK DlssNrHoverProc(HWND h, UINT msg, WPARAM wp, LPARAM lp,
                                        UINT_PTR, DWORD_PTR) {
    switch (msg) {
    case WM_MOUSEMOVE: {
        DlssNrSetHot(h, true);
        TRACKMOUSEEVENT tme = { sizeof(tme), TME_LEAVE, h, 0 };
        TrackMouseEvent(&tme);
        break;
    }
    case WM_MOUSELEAVE:
        DlssNrSetHot(h, false);
        break;
    case WM_ERASEBKGND:
        return 1;
    }
    return DefSubclassProc(h, msg, wp, lp);
}

static void DlssNrAttachHover(HWND h) {
    if (h) SetWindowSubclass(h, DlssNrHoverProc, 1, 0);
}

// The layer strip has two independent visibility inputs -- which page is up and
// how many layers the chain has -- so it gets one function. A page switch that
// blindly showed every strip button is exactly how the buttons of layers that
// were just removed come back. It also owns the stepper's enabled state and the
// N readout, because those change for the same two reasons.
static void DlssNrSyncLayerStrip(DlssNrSettings* s) {
    if (!s) return;
    Engine& e = Engine::Instance();
    const int n = e.OptLayerCount();
    const int sel = e.OptSelLayer();
    const bool pageVisible = (s->page == 0 || s->page == 2);
    for (int i = 0; i < (int)dlssnr::kMaxLayers; ++i) {
        HWND b = s->layerBtn[i];
        if (!b) continue;
        ShowWindow(b, (pageVisible && i < n) ? SW_SHOW : SW_HIDE);
        // A one-layer chain has nothing to switch to. Keep the button visible and
        // draw it inert instead of hiding it, so the feature stays discoverable
        // for someone who has only ever run the single-layer default.
        EnableWindow(b, (n > 1) ? TRUE : FALSE);
        InvalidateRect(b, nullptr, FALSE);
    }
    EnableWindow(s->btnLayerMinus, (n > 1) ? TRUE : FALSE);
    EnableWindow(s->btnLayerPlus,  (n < (int)dlssnr::kMaxLayers) ? TRUE : FALSE);
    wchar_t t[16];
    _snwprintf_s(t, _countof(t), _TRUNCATE, L"%d", n);
    SetWindowTextW(s->lblLayerCount, t);
    s->shownLayerCount = n;
    s->shownSelLayer = sel;
}

// A model segment's label. The raw-id segment shows the id that is (or, while a
// fixed segment is selected, would be) in effect, so the button explains itself
// without a tooltip; the others are the table's names.
static const wchar_t* DlssNrModelSegLabel(int seg, int cur, wchar_t* buf, int cch) {
    if (seg == kDlssNrModelBtnCustom) {
        const int id = (cur >= kDlssNrModelBtnCustom) ? dlssnr::ModelClamp(cur)
                                                      : DlssNrModelCustomNext(cur);
        if (g_uiLang == 1) _snwprintf_s(buf, (size_t)cch, _TRUNCATE, L"Preset %d", id);
        else               _snwprintf_s(buf, (size_t)cch, _TRUNCATE, L"预设 %d", id);
        return buf;
    }
    const dlssnr::ModelInfo* m = dlssnr::ModelFind(DlssNrModelSegmentPreset(seg, cur));
    if (m) return (g_uiLang == 1) ? m->en : m->zh;
    return L"";
}

// Enable the model segments for what the loaded nvngx_dlssnr.dll can actually
// serve. Preset 0 (the DLL's default) and the raw-id segment are always usable;
// a fixed segment whose resource this DLL does not carry is drawn INERT rather
// than hidden -- the same choice the layer strip makes -- so the row keeps its
// shape (a different DLL must not resize the panel) and the absence is visible
// instead of silent. The status page's model line names what the DLL has.
static void DlssNrSyncModelButtons(DlssNrSettings* s) {
    if (!s) return;
    dlssnr::DllWeight w[dlssnr::kDllWeightMax];
    const int n = dlssnr::DllWeights(w, dlssnr::kDllWeightMax);
    const int cur = Engine::Instance().OptModel();
    for (int i = 0; i < kDlssNrModelBtnCount; ++i) {
        if (!s->modelBtn[i]) continue;
        const bool can = (i == kDlssNrModelBtnCustom) ||
                         DlssNrModelAvailable(DlssNrModelSegmentPreset(i, cur), w, n);
        EnableWindow(s->modelBtn[i], can ? TRUE : FALSE);
        InvalidateRect(s->modelBtn[i], nullptr, FALSE);
    }
    s->shownModel = cur;
}

// ---- layout ----------------------------------------------------------------
static void DlssNrLayout(DlssNrSettings* s) {
    const DlssNrMetrics& m = DlssNrGetMetrics();
    const int x0 = m.margin;
    const int right = m.wantW - m.margin;

    // ---- own title bar (borderless standalone window only) ----
    const int headH = s->standalone ? m.headerH : 0;
    if (s->btnClose) {
        MoveWindow(s->btnClose, right - m.closeW, (headH - m.closeH) / 2,
                   m.closeW, m.closeH, TRUE);
    }

    // ---- tab strip (both tabs are always visible) ----
    int y = headH + m.gap;
    MoveWindow(s->tab[0], x0, y, m.tabW, m.tabH, TRUE);
    MoveWindow(s->tab[1], x0 + m.tabW + m.btnGap, y, m.tabW, m.tabH, TRUE);
    MoveWindow(s->tab[2], x0 + 2 * (m.tabW + m.btnGap), y, m.tabW, m.tabH, TRUE);
    const int langX = right - 2 * m.langW - m.btnGap;
    MoveWindow(s->lang[0], langX, y, m.langW, m.tabH, TRUE);
    MoveWindow(s->lang[1], langX + m.langW + m.btnGap, y, m.langW, m.tabH, TRUE);

    // ---- parameters page ----
    y += m.tabH + m.gap;
    // Layer row first: it scopes EVERYTHING under it (the style segments, the
    // four sliders and the auto-mask box all edit the selected layer), so it has
    // to read as the outermost control, not as a sidebar to the sliders.
    {
        const int ry = y + (m.layerRowH - m.rowH) / 2;
        int cx = x0;
        MoveWindow(s->lblLayerName, cx, ry, m.lblW, m.rowH, TRUE);          cx += m.lblW + m.gap;
        MoveWindow(s->btnLayerMinus, cx, y, m.layerStepW, m.layerRowH, TRUE); cx += m.layerStepW;
        MoveWindow(s->lblLayerCount, cx, ry, m.layerCountW, m.rowH, TRUE);    cx += m.layerCountW;
        MoveWindow(s->btnLayerPlus, cx, y, m.layerStepW, m.layerRowH, TRUE);  cx += m.layerStepW + m.gap;
        for (int i = 0; i < (int)dlssnr::kMaxLayers; ++i) {
            MoveWindow(s->layerBtn[i], cx, y, m.layerBtnW, m.layerRowH, TRUE);
            cx += m.layerBtnW + m.btnGap;
        }
    }
    y += m.layerRowH + m.gap;
    MoveWindow(s->lblLayerNote, x0, y, right - x0, m.layerNoteH, TRUE);
    y += m.layerNoteH + m.gap;

    // ---- style row ----
    int x = x0 + m.lblW + m.gap;
    for (int i = 0; i < kDlssNrStyleCount; ++i) {
        MoveWindow(s->styleBtn[i], x, y, m.styleW, m.segH, TRUE);
        x += m.styleW + m.btnGap;
    }
    y += m.segH + m.gap;

    const int rowTop = y;
    for (int i = 0; i < kDlssNrSliderCount; ++i) {
        const int ry = rowTop + i * m.rowPitch + (m.rowPitch - m.rowH) / 2;
        int cx = x0;
        MoveWindow(s->lbl[i], cx, ry, m.lblW, m.rowH, TRUE);          cx += m.lblW + m.gap;
        MoveWindow(s->trk[i], cx, ry, m.barW, m.rowH, TRUE);          cx += m.barW + m.gap;
        MoveWindow(s->val[i], cx, ry, m.valW, m.rowH, TRUE);          cx += m.valW + m.gap;
        MoveWindow(s->rst[i], cx, ry, m.rstW, m.rowH, TRUE);
    }
    // The processing-resolution row: label / slider / readout. No Reset button --
    // 100% IS the reset, and it is where switching the feature off sits.
    {
        const int ry = rowTop + kDlssNrSliderCount * m.rowPitch + (m.rowPitch - m.rowH) / 2;
        int cx = x0;
        MoveWindow(s->lblProc, cx, ry, m.lblW, m.rowH, TRUE);  cx += m.lblW + m.gap;
        MoveWindow(s->trkProc, cx, ry, m.barW, m.rowH, TRUE);  cx += m.barW + m.gap;
        MoveWindow(s->valProc, cx, ry, m.valW, m.rowH, TRUE);  cx += m.valW + m.gap;
        MoveWindow(s->rstProc, cx, ry, m.rstW, m.rowH, TRUE);
    }
    // The extrapolation row: label / slider / readout, directly under the
    // processing-resolution row because both are session-level knobs that are
    // not per-layer parameters.
    {
        const int ry = rowTop + (kDlssNrSliderCount + 1) * m.rowPitch
                     + (m.rowPitch - m.rowH) / 2;
        int cx = x0;
        MoveWindow(s->lblExtrap, cx, ry, m.lblW, m.rowH, TRUE);  cx += m.lblW + m.gap;
        MoveWindow(s->trkExtrap, cx, ry, m.barW, m.rowH, TRUE);  cx += m.barW + m.gap;
        MoveWindow(s->valExtrap, cx, ry, m.valW, m.rowH, TRUE);  cx += m.valW + m.gap;
        MoveWindow(s->rstExtrap, cx, ry, m.rstW, m.rowH, TRUE);
    }
    y = rowTop + (kDlssNrSliderCount + 2) * m.rowPitch + m.gap;

    // ---- model row: which network inside nvngx_dlssnr.dll ----
    // Session-level exactly like the processing-resolution row above it (both are
    // feature-creation settings that rebuild the session), so it sits next to it
    // rather than among the per-layer controls.
    {
        const int ry = y + (m.segH - m.rowH) / 2;
        int cx = x0;
        MoveWindow(s->lblModel, cx, ry, m.lblW, m.rowH, TRUE);
        cx += m.lblW + m.gap;
        for (int i = 0; i < kDlssNrModelBtnCount; ++i) {
            MoveWindow(s->modelBtn[i], cx, y, m.modelBtnW, m.segH, TRUE);
            cx += m.modelBtnW + m.btnGap;
        }
    }
    y += m.segH + m.gap;

    HWND checks[5] = { s->chkEnable, s->chkAutoMask, s->chkDrop, s->chkProc, s->chkAutoshow };
    x = x0;                                   // row 1: what the picture looks like
    for (int i = 0; i < 3; ++i) {
        MoveWindow(checks[i], x, y, m.chkW[i], m.rowH, TRUE);
        x += m.chkW[i] + m.gap;
    }
    y += m.rowH + m.gap / 2;                  // row 2: resolution + panel prefs
    x = x0;
    for (int i = 3; i < 5; ++i) {
        MoveWindow(checks[i], x, y, m.chkW[i], m.rowH, TRUE);
        x += m.chkW[i] + m.gap;
    }
    y += m.rowH + m.gap;

    MoveWindow(s->btnResetAll, x0, y, m.resetAllW, m.segH, TRUE);
    int rx = right - m.logW;
    MoveWindow(s->btnLog, rx, y, m.logW, m.segH, TRUE);
    rx -= m.gap + m.recheckW;
    MoveWindow(s->btnRecheck, rx, y, m.recheckW, m.segH, TRUE);
    y += m.segH + m.gap;
    MoveWindow(s->lblHint, x0, y, right - x0, m.hintH, TRUE);

    // ---- status page ----
    int sy = headH + m.gap + m.tabH + m.gap;
    MoveWindow(s->lblTitle, x0, sy, right - x0, m.titleH, TRUE);
    sy += m.titleH + m.gap;
    MoveWindow(s->lblSub,   x0, sy, right - x0, m.lineH, TRUE); sy += m.lineH + m.gap;
    MoveWindow(s->lblStats, x0, sy, right - x0, m.lineH, TRUE); sy += m.lineH + m.gap;
    MoveWindow(s->lblGeom,  x0, sy, right - x0, m.lineH, TRUE); sy += m.lineH + m.gap;
    MoveWindow(s->lblLayers, x0, sy, right - x0, m.lineH, TRUE); sy += m.lineH + m.gap;
    MoveWindow(s->lblModelLine, x0, sy, right - x0, m.lineH, TRUE);

    // ---- page visibility (the compare page has no controls of its own) ----
    // The compare page ALSO shows the parameter controls: the whole point is to drag a
    // slider while watching the split view, so everything must be reachable there.
    const int show = (s->page == 0 || s->page == 2) ? SW_SHOW : SW_HIDE;
    const int hide = (s->page == 0) ? SW_HIDE : SW_SHOW;
    for (int i = 0; i < kDlssNrStyleCount; ++i) ShowWindow(s->styleBtn[i], show);
    for (int i = 0; i < kDlssNrSliderCount; ++i) {
        ShowWindow(s->lbl[i], show);
        ShowWindow(s->trk[i], show);
        ShowWindow(s->val[i], show);
        ShowWindow(s->rst[i], show);
    }
    // The processing-resolution row belongs to the parameter page too (and to the
    // compare page, which shares it). Missing these three lines is how it ended up
    // drawn on the STATUS page, in the middle of the telemetry.
    ShowWindow(s->lblProc, show);
    ShowWindow(s->trkProc, show);
    ShowWindow(s->valProc, show);
    // Same trap as the three lines above: leave these out and the extrapolation
    // row paints itself across the status page.
    ShowWindow(s->lblExtrap, show);
    ShowWindow(s->trkExtrap, show);
    ShowWindow(s->valExtrap, show);
    ShowWindow(s->rstProc, show);
    ShowWindow(s->rstExtrap, show);
    // The model row belongs to the parameter page too, for the same reason the
    // processing-resolution row does. Its segments are ENABLED per what the loaded
    // DLL carries (DlssNrSyncModelButtons), not shown/hidden: the row must keep
    // its shape so a different DLL cannot resize the panel.
    ShowWindow(s->lblModel, show);
    for (int i = 0; i < kDlssNrModelBtnCount; ++i) ShowWindow(s->modelBtn[i], show);
    for (HWND h : checks) ShowWindow(h, show);
    // The layer row belongs to the parameter page (and to the compare page, which
    // shares it). The strip buttons themselves are shown per layer COUNT by
    // DlssNrSyncLayerStrip(), so they are deliberately not in this group.
    ShowWindow(s->lblLayerName, show);
    ShowWindow(s->lblLayerCount, show);
    ShowWindow(s->btnLayerMinus, show);
    ShowWindow(s->btnLayerPlus, show);
    ShowWindow(s->lblLayerNote, show);
    ShowWindow(s->btnResetAll, show);
    ShowWindow(s->btnRecheck, show);
    ShowWindow(s->btnLog, show);
    ShowWindow(s->lblHint, show);
    const int statusShow = (s->page == 1) ? SW_SHOW : SW_HIDE;
    ShowWindow(s->lblTitle, statusShow);
    ShowWindow(s->lblSub, statusShow);
    ShowWindow(s->lblStats, statusShow);
    ShowWindow(s->lblGeom, statusShow);
    ShowWindow(s->lblLayers, statusShow);
    ShowWindow(s->lblModelLine, statusShow);
    DlssNrSyncLayerStrip(s);
    DlssNrSyncModelButtons(s);
}

// ---- text ------------------------------------------------------------------
static COLORREF g_statusColour = RGB(0x96, 0x96, 0x96);

static const wchar_t* DlssNrHintText() {
    return T(kDlssNrHintZh, kDlssNrHintEn);
}

// Defined with the compare window further down; the panel re-titles that window on a
// language switch, which is why the declaration is needed here.
static const wchar_t* DlssNrCompareTitle();

static void DlssNrRetext(HWND hwnd) {
    DlssNrSettings* s = (DlssNrSettings*)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
    if (!s) return;
    SetWindowTextW(hwnd, s->standalone ? T(L"DLSSNR 控制面板", L"DLSSNR Control Panel")
                                       : T(L"DLSSNR", L"DLSSNR"));
    SetWindowTextW(s->lbl[0], T(L"强度",     L"Intensity"));
    SetWindowTextW(s->lbl[1], T(L"局部色调", L"Local tone"));
    SetWindowTextW(s->lbl[2], T(L"局部结构", L"Local structure"));
    SetWindowTextW(s->lbl[3], T(L"皮肤结构", L"Skin structure"));
    SetWindowTextW(s->lblProc, T(kDlssNrProcLblZh, kDlssNrProcLblEn));
    SetWindowTextW(s->lblExtrap, T(kDlssNrExtrapLblZh, kDlssNrExtrapLblEn));
    SetWindowTextW(s->lblModel, T(kDlssNrModelLblZh, kDlssNrModelLblEn));
    SetWindowTextW(s->lblLayerName, T(kDlssNrLayersLblZh, kDlssNrLayersLblEn));
    SetWindowTextW(s->lblLayerNote, T(kDlssNrLayerNoteZh, kDlssNrLayerNoteEn));
    SetWindowTextW(s->lblHint, DlssNrHintText());
    if (s->compareWnd) {
        SetWindowTextW(s->compareWnd, DlssNrCompareTitle());
        InvalidateRect(s->compareWnd, nullptr, FALSE);
    }
    RedrawWindow(hwnd, nullptr, nullptr,
                 RDW_INVALIDATE | RDW_ALLCHILDREN | RDW_UPDATENOW);
}

static const wchar_t* DlssNrButtonLabel(int id) {
    switch (id) {
    case IDC_DLS_TAB0:      return T(L"参数", L"Parameters");
    case IDC_DLS_TAB0 + 1:  return T(L"状态", L"Status");
    case IDC_DLS_TAB2:      return T(L"对比", L"Compare");
    case IDC_DLS_LANG0:     return L"中文";
    case IDC_DLS_LANG0 + 1: return L"English";
    case IDC_DLS_ENABLE:    return T(L"启用 DLSSNR", L"Enable DLSSNR");
    case IDC_DLS_AUTOMASK:  return T(L"自动遮罩", L"Automatic mask");
    case IDC_DLS_AUTOSHOW:  return T(L"启动时显示控制面板", L"Show panel on playback");
    case IDC_DLS_DROP:      return T(kDlssNrDropZh, kDlssNrDropEn);
    case IDC_DLS_PROCCHK:   return T(kDlssNrProcChkZh, kDlssNrProcChkEn);
    case IDC_DLS_RESETALL:  return T(L"重置为默认", L"Reset defaults");
    case IDC_DLS_RECHECK:   return T(L"重新检测", L"Recheck");
    case IDC_DLS_LOG:       return T(L"打开日志", L"Open log");
    case IDC_DLS_CLOSE:     return L"\x00D7";      // x (U+00D7: in every UI font)
    case IDC_DLS_LCOUNT_MINUS: return L"-";   // ASCII: present in every UI font
    case IDC_DLS_LCOUNT_PLUS:  return L"+";
    default: break;
    }
    if (id >= IDC_DLS_STYLE0 && id < IDC_DLS_STYLE0 + kDlssNrStyleCount)
        return DlssNrStyleName(id - IDC_DLS_STYLE0);
    if (id >= IDC_DLS_MODEL0 && id < IDC_DLS_MODEL0 + kDlssNrModelBtnCount) {
        // The raw-id segment's label depends on what the panel currently holds, so
        // it needs static storage: the label is read while the button paints, and
        // returning a pointer into a local would dangle immediately -- the same
        // reason the layer digits below are static.
        static wchar_t modelLabel[kDlssNrModelBtnCount][24];
        const int seg = id - IDC_DLS_MODEL0;
        return DlssNrModelSegLabel(seg, Engine::Instance().OptModel(),
                                   modelLabel[seg], _countof(modelLabel[seg]));
    }
    if (id >= IDC_DLS_LAYER0 && id < IDC_DLS_LAYER0 + (int)dlssnr::kMaxLayers) {
        // A digit per layer. Static storage: the label is read while the button
        // paints, and returning a pointer into a local would dangle immediately.
        static wchar_t digits[dlssnr::kMaxLayers][4];
        const int i = id - IDC_DLS_LAYER0;
        _snwprintf_s(digits[i], _countof(digits[i]), _TRUNCATE, L"%d", i + 1);
        return digits[i];
    }
    if (id >= IDC_DLS_RST0 && id < IDC_DLS_RST0 + kDlssNrSliderCount * 10 &&
        (id - IDC_DLS_RST0) % 10 == 0)
        return T(L"重置", L"Reset");
    if (id == IDC_DLS_PROCRST || id == IDC_DLS_EXTRAPRST)
        return T(L"重置", L"Reset");
    return L"";
}

static void DlssNrRefreshStatus(HWND hwnd) {
    DlssNrSettings* s = (DlssNrSettings*)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
    if (!s) return;
    if (!g_shared) {
        g_statusColour = kClrTextDim;
        SetWindowTextW(s->lblTitle, T(L"未连接", L"Not connected"));
        SetWindowTextW(s->lblSub,   T(L"共享内存不可用", L"Shared memory unavailable"));
        SetWindowTextW(s->lblStats, L"");
        SetWindowTextW(s->lblGeom,  L"");
        SetWindowTextW(s->lblLayers, L"");
        InvalidateRect(s->lblTitle, nullptr, TRUE);
        return;
    }
    SharedState* sh = g_shared;
    const wchar_t* eng = sh->engineReady ? T(L"就绪", L"ready")
                      : (sh->engineGaveUp ? T(L"失败", L"failed") : T(L"加载中", L"loading"));
    const wchar_t* colourTxt;
    COLORREF col = kClrTextDim;
    switch ((int)sh->lastReason) {
    case 0: colourTxt = T(L"正在处理", L"Processing");                 col = kClrOk; break;
    case 1: colourTxt = T(L"引擎加载中…（约 1-15 秒）", L"Loading model… (1-15 s)"); col = kClrWarn; break;
    case 2: colourTxt = T(L"已关闭（面板或 ini）", L"Disabled (panel or ini)");     col = kClrTextDim; break;
    case 4: colourTxt = T(L"分辨率与引擎会话不一致", L"Resolution differs from engine session"); col = kClrErr; break;
    case 5: colourTxt = T(L"引擎返回错误，已直通", L"Engine error, passing through"); col = kClrErr; break;
    case 6: colourTxt = T(L"引擎初始化失败（详见日志）", L"Engine init failed (see log)"); col = kClrErr; break;
    default: colourTxt = T(L"未知状态", L"Unknown");                    col = kClrTextDim; break;
    }
    if ((int)sh->heartbeat == 0) colourTxt = T(L"等待滤镜开始处理…", L"Waiting for the filter…");
    g_statusColour = col;
    SetWindowTextW(s->lblTitle, colourTxt);
    InvalidateRect(s->lblTitle, nullptr, TRUE);   // the colour can change on its own

    wchar_t buf[320];
    _snwprintf_s(buf, _countof(buf), _TRUNCATE, L"PID %d    %s", (int)sh->pid, sh->status);
    SetWindowTextW(s->lblSub, buf);

    const int seen = (int)sh->framesSeen, proc = (int)sh->framesProcessed;
    // "dropped" is the late-frame policy's own counter (see ShouldDropLateFrame):
    // frames the engine was too slow for, and that would only have pushed the
    // picture further behind the sound. Frames are counted cumulatively for the
    // whole process, so a running percentage lives in tools\read_telemetry.py.
    _snwprintf_s(buf, _countof(buf), _TRUNCATE,
                 T(L"帧: 收到 %d / 处理 %d / 直通 %d / 丢帧 %d    引擎 %s    %.2f ms/帧",
                   L"Frames: seen %d / processed %d / passthrough %d / dropped %d    engine %s    %.2f ms/frame"),
                 seen, proc, (int)sh->framesPassthrough, (int)sh->framesDropped,
                 eng, (double)sh->lastProcessMs);
    SetWindowTextW(s->lblStats, buf);

    // "engine session" IS the processing size now, so show the applied
    // percentage with it -- that is how a fallback (asked for 25%, got native
    // because the size was not usable) becomes visible instead of silent.
    Engine& en = Engine::Instance();
    const int pct = en.Ready() ? en.AppliedScale() : en.ProcScalePct();
    const int pw  = en.Ready() ? en.AppliedW() : 0;
    const int ph2 = en.Ready() ? en.AppliedH() : 0;
    _snwprintf_s(buf, _countof(buf), _TRUNCATE,
                 T(L"视频 %dx%d    %s    处理 %d%% (%dx%d)    滞后 %.1f ms",
                   L"Video %dx%d    %s    proc %d%% (%dx%d)    lag %.1f ms"),
                 (int)sh->videoW, (int)sh->videoH, DlssNrFmtName((int)sh->inputBpp),
                 pct, pw, ph2, (double)sh->dropLagMs);
    SetWindowTextW(s->lblGeom, buf);

    // Configured vs actually built. They differ for the frame or two between
    // raising the count and the next frame creating the extra NGX features, and
    // that gap is exactly what "the chain is N passes" needs to be checked
    // against. Read from the in-process engine, not the shared block: the panel
    // drives this engine directly and can change it while a paused graph is not
    // publishing telemetry at all.
    const int nCfg  = en.OptLayerCount();
    const int nLive = dlssnr::ActiveLayers();
    _snwprintf_s(buf, _countof(buf), _TRUNCATE,
                 T(L"层: 配置 %d 层 / 引擎已建 %d 层    每层是一次完整推理",
                   L"Layers: %d configured / %d built    one full pass per layer"),
                 nCfg, nLive);
    SetWindowTextW(s->lblLayers, buf);

    // Which network inside nvngx_dlssnr.dll this session is running. Three facts
    // in one line, because "the switch did nothing" has three different causes and
    // the line has to tell them apart:
    //   * the DLL's version and the resources it actually carries (the panel's
    //     segments are enabled from the same scan, so a greyed-out segment has a
    //     stated reason here),
    //   * the request (panel/ini) and what the engine APPLIED -- they differ when
    //     the DLL refused the preset and the engine fell back to its default.
    {
        wchar_t ver[32] = L"";
        dlssnr::DllVersion(ver, _countof(ver));
        dlssnr::DllWeight w[dlssnr::kDllWeightMax];
        const int nw = dlssnr::DllWeights(w, dlssnr::kDllWeightMax);
        wchar_t list[160] = L"";
        for (int i = 0; i < nw; ++i) {
            const wchar_t* nm = L"";
            wchar_t tmp[64];
            // Show the short panel name when the table knows the resource, and the
            // resource's own tail when it does not -- an unknown network must be
            // visible, because that is the one a raw preset id can select.
            if (w[i].preset >= 0) nm = DlssNrModelSegLabel(
                (w[i].preset < kDlssNrModelBtnCustom) ? w[i].preset : kDlssNrModelBtnCustom,
                w[i].preset, tmp, _countof(tmp));
            else {
                const char* tail = w[i].name;
                const char* us = strrchr(w[i].name, '_');
                if (us && us[1]) tail = us + 1;
                _snwprintf_s(tmp, _countof(tmp), _TRUNCATE, L"%hs", tail);
                nm = tmp;
            }
            if (list[0]) wcscat_s(list, L", ");
            wcscat_s(list, nm);
        }
        // NOTE: take the RETURNED pointer, not the buffer. A fixed segment returns
        // its table label and never touches `buf`, so printing the buffer here
        // printed uninitialised stack -- which is exactly how this line first came
        // out as two garbage code units instead of "自动".
        wchar_t reqBuf[32], appBuf[32];
        const int reqSeg = (en.OptModel() < kDlssNrModelBtnCustom)
                               ? en.OptModel() : kDlssNrModelBtnCustom;
        const int appSeg = (en.AppliedModel() < kDlssNrModelBtnCustom)
                               ? en.AppliedModel() : kDlssNrModelBtnCustom;
        const wchar_t* reqLbl = DlssNrModelSegLabel(reqSeg, en.OptModel(),
                                                    reqBuf, _countof(reqBuf));
        const wchar_t* appLbl = DlssNrModelSegLabel(appSeg, en.AppliedModel(),
                                                    appBuf, _countof(appBuf));
        _snwprintf_s(buf, _countof(buf), _TRUNCATE,
                     T(L"模型: %s (%d)    引擎实际: %s (%d)%s    DLL %s 网络: %s",
                       L"Model: %s (%d)    applied: %s (%d)%s    DLL %s nets: %s"),
                     reqLbl, en.OptModel(), appLbl, en.AppliedModel(),
                     en.ModelFellBack() ? T(L"  (!) 该 DLL 拒绝了请求，已回退",
                                            L"  (!) DLL refused, fell back") : L"",
                     ver[0] ? ver : L"?", list[0] ? list : T(L"未检测到", L"none found"));
        SetWindowTextW(s->lblModelLine, buf);
    }
}

static void DlssNrRefreshControls(HWND hwnd) {
    DlssNrSettings* s = (DlssNrSettings*)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
    if (!s) return;
    Engine& e = Engine::Instance();
    DlssNrSetChecked(s->chkEnable, e.OptEnabled());
    DlssNrSetChecked(s->chkAutoMask, e.OptAutoMask());
    DlssNrSetChecked(s->chkDrop, e.OptDropFrames());
    DlssNrSetChecked(s->chkProc, e.OptProcEnabled());
    // The slider keeps its position while the switch is off (that is the value
    // the panel restores), but it is not editable until the switch is on.
    EnableWindow(s->trkProc, e.OptProcEnabled() ? TRUE : FALSE);
    SendMessageW(s->trkProc, TBM_SETPOS, TRUE, e.OptProcScale());
    {
        wchar_t t[16];
        _snwprintf_s(t, _countof(t), _TRUNCATE, L"%d%%", e.OptProcScale());
        SetWindowTextW(s->valProc, t);
        InvalidateRect(s->trkProc, nullptr, FALSE);
    }
    {
        const int ex = e.OptExtrap();
        SendMessageW(s->trkExtrap, TBM_SETPOS, TRUE, ex);
        wchar_t t[16];
        // 100 % reads as "off", because that is what it is -- the row must not
        // look like a second strength control that is merely at its default.
        if (ex == 100) _snwprintf_s(t, _countof(t), _TRUNCATE, L"%s", T(L"关", L"off"));
        else           _snwprintf_s(t, _countof(t), _TRUNCATE, L"%d%%", ex);
        SetWindowTextW(s->valExtrap, t);
        InvalidateRect(s->trkExtrap, nullptr, FALSE);
    }
    DlssNrSetChecked(s->chkAutoshow, DlssNrAutoshow());
    const float v[kDlssNrSliderCount] = {
        e.OptIntensity(), e.OptTone(), e.OptStruct(), e.OptSkin() };
    for (int i = 0; i < kDlssNrSliderCount; ++i) {
        const int pct = DlssNrPctMax(v[i], kDlssNrSliderMax[i]);
        SendMessageW(s->trk[i], TBM_SETPOS, TRUE, pct);
        wchar_t t[16];
        _snwprintf_s(t, _countof(t), _TRUNCATE, L"%d%%", pct);
        SetWindowTextW(s->val[i], t);
        InvalidateRect(s->trk[i], nullptr, FALSE);
    }
    s->shownStyle = e.OptStyle();
    for (int i = 0; i < kDlssNrStyleCount; ++i) InvalidateRect(s->styleBtn[i], nullptr, FALSE);
    // Which layer the controls above were just loaded from, and the count/strip
    // that go with it. Kept last so the strip highlight matches the values.
    DlssNrSyncLayerStrip(s);
}

// Grow or shrink the chain from the UI. SetLayerCount() only records the new
// count -- the extra NGX features are built lazily on the next frame -- so the
// panel's on-screen values go stale the moment this runs: growing copies the
// last layer into the new slots INSIDE the engine, and those copies are what the
// user must now see. Hence refresh + persist + tell a paused frame to re-render.
static void DlssNrSetLayerCountFromUi(HWND hwnd, int n) {
    DlssNrSettings* s = (DlssNrSettings*)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
    if (!s) return;
    Engine& e = Engine::Instance();
    const int oldCount = e.OptLayerCount();
    n = n < 1 ? 1 : (n > (int)dlssnr::kMaxLayers ? (int)dlssnr::kMaxLayers : n);
    if (n == oldCount) return;
    e.SetLayerCount(n);
    // Growing means "do that again, a bit softer", not "keep editing the old
    // one": the engine already copied the last layer into the new slots, so make
    // the top one current. Shrinking keeps the selection the engine clamped.
    if (n > oldCount) e.SetSelLayer(n - 1);
    DlssNrWriteLayers();          // layers=N + every active layer's six keys
    DlssNrRefreshControls(hwnd);  // the new slots hold copies: never show stale rows
    DlssNrRefreshSinks(true);     // a paused graph must build the extra features
    if (s->onChanged) s->onChanged(s->onChangedCtx);
    HWND par = GetParent(hwnd);
    if (par) PostMessageW(par, DLS_N_CHANGED, 0, 0);
}

static void DlssNrPush(HWND hwnd, bool persist) {
    DlssNrSettings* s = (DlssNrSettings*)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
    if (!s) return;
    int pct[kDlssNrSliderCount];
    for (int i = 0; i < kDlssNrSliderCount; ++i) {
        pct[i] = (int)SendMessageW(s->trk[i], TBM_GETPOS, 0, 0);
        wchar_t t[16];
        _snwprintf_s(t, _countof(t), _TRUNCATE, L"%d%%", pct[i]);
        SetWindowTextW(s->val[i], t);
        InvalidateRect(s->rst[i], nullptr, FALSE);
    }
    DlssNrApply(DlssNrChecked(s->chkEnable), Engine::Instance().OptStyle(),
                pct[0], pct[1], pct[2], pct[3], DlssNrChecked(s->chkAutoMask), persist);
    if (s->onChanged) s->onChanged(s->onChangedCtx);
    HWND par = GetParent(hwnd);
    if (par) PostMessageW(par, DLS_N_CHANGED, 0, 0);
}

// ---------------------------------------------------------------------------
// The 对比 (compare) window
//
// Shows the frame the filter kept, twice: as the graph delivered it (left) and as the
// engine renders it (right), split by a draggable divider -- the same idea as NVIDIA
// ICAT's split view. This is the ONE place where a parameter change made while the
// player is paused can be seen: the filter re-runs the frame it kept whenever the
// panel reports an edit (see RefreshIfNeeded in the filter), and this window repaints
// from that buffer. Nothing here touches the player's graph, which is why it works.
// ---------------------------------------------------------------------------
static int DlssNrScaled(int v) {
    return (int)(v * (DlssNrGetMetrics().lineH / 16.0f) + 0.5f);
}

struct DlssNrCompare {
    HWND  hwnd;
    int   split;        // divider position, percent of the client width
    bool  dragging;
    LONG  seq;          // last preview revision drawn
};
static DlssNrCompare g_cmp = { nullptr, 50, false, -1 };

// The font the compare window draws its labels with (created lazily).
static HFONT DlssNrCompareFont() {
    static HFONT f = nullptr;
    if (!f) {
        f = CreateFontW(-DlssNrGetMetrics().lineH, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
                        DEFAULT_CHARSET, OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS,
                        CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
    }
    return f;
}

static void DlssNrCompareSetSplit(HWND hwnd, int x) {
    RECT rc;
    GetClientRect(hwnd, &rc);
    if (rc.right <= 0) return;
    int pct = (int)((long long)x * 100 / rc.right);
    if (pct < 2)  pct = 2;
    if (pct > 98) pct = 98;
    if (pct == g_cmp.split) return;
    g_cmp.split = pct;
    InvalidateRect(hwnd, nullptr, FALSE);
}

static void DlssNrComparePaint(HWND hwnd) {
    PAINTSTRUCT ps;
    HDC wdc = BeginPaint(hwnd, &ps);
    RECT rc;
    GetClientRect(hwnd, &rc);
    // Double buffered: repainting straight to the window flashes on every update, which
    // is what the first version of this window showed.
    HDC dc = CreateCompatibleDC(wdc);
    HBITMAP bmp = CreateCompatibleBitmap(wdc, rc.right > 0 ? rc.right : 1,
                                              rc.bottom > 0 ? rc.bottom : 1);
    HGDIOBJ oldBmp = bmp ? SelectObject(dc, bmp) : nullptr;
    FillRect(dc, &rc, DlssNrBrush(kClrBg));

    const int w = DlssNrPreviewW(), h = DlssNrPreviewH();
    const int pad = DlssNrScaled(8);
    RECT area = { pad, pad, rc.right - pad, rc.bottom - pad };
    if (w <= 0 || h <= 0 || area.right <= area.left || area.bottom <= area.top) {
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, kClrTextDim);
        SelectObject(dc, DlssNrCompareFont());
        const wchar_t* msg = T(L"还没有可对比的画面：先播放一小会儿，让滤镜拿到一帧。",
                               L"No frame yet: play for a moment so the filter has one.");
        DrawTextW(dc, msg, -1, &area, DT_CENTER | DT_VCENTER | DT_WORDBREAK | DT_NOPREFIX);
        BitBlt(wdc, 0, 0, rc.right, rc.bottom, dc, 0, 0, SRCCOPY);
        if (oldBmp) SelectObject(dc, oldBmp);
        if (bmp) DeleteObject(bmp);
        DeleteDC(dc);
        EndPaint(hwnd, &ps);
        return;
    }

    // Letterbox: keep the frame's aspect ratio inside the client area.
    int dw = area.right - area.left, dh = area.bottom - area.top;
    if ((long long)dw * h > (long long)dh * w) dw = (int)((long long)dh * w / h);
    else                                        dh = (int)((long long)dw * h / w);
    RECT dst = { area.left + ((area.right - area.left) - dw) / 2,
                 area.top  + ((area.bottom - area.top) - dh) / 2, 0, 0 };
    dst.right = dst.left + dw;
    dst.bottom = dst.top + dh;

    BITMAPINFO bi;
    ZeroMemory(&bi, sizeof(bi));
    bi.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth       = w;
    bi.bmiHeader.biHeight      = -h;          // top-down, like our buffer
    bi.bmiHeader.biPlanes      = 1;
    bi.bmiHeader.biBitCount    = 24;          // DirectShow RGB24 is already B,G,R
    bi.bmiHeader.biCompression = BI_RGB;

    const int splitX = dst.left + dw * g_cmp.split / 100;
    DlssNrPreviewLock();
    const BYTE* orig = DlssNrPreviewOrig();
    const BYTE* proc = DlssNrPreviewProc();
    if (orig && proc) {
        SetStretchBltMode(dc, HALFTONE);
        SetBrushOrgEx(dc, 0, 0, nullptr);
        const int save = SaveDC(dc);
        IntersectClipRect(dc, dst.left, dst.top, splitX, dst.bottom);
        StretchDIBits(dc, dst.left, dst.top, dw, dh, 0, 0, w, h, orig, &bi,
                      DIB_RGB_COLORS, SRCCOPY);
        RestoreDC(dc, save);
        const int save2 = SaveDC(dc);
        IntersectClipRect(dc, splitX, dst.top, dst.right, dst.bottom);
        StretchDIBits(dc, dst.left, dst.top, dw, dh, 0, 0, w, h, proc, &bi,
                      DIB_RGB_COLORS, SRCCOPY);
        RestoreDC(dc, save2);
    }
    DlssNrPreviewUnlock();

    // Divider + handle, like ICAT's.
    HBRUSH line = DlssNrBrush(RGB(0xE8, 0xE8, 0xE8));
    RECT bar = { splitX - DlssNrScaled(1), dst.top, splitX + DlssNrScaled(1) + 1, dst.bottom };
    FillRect(dc, &bar, line);
    const int hs = DlssNrScaled(9);
    HBRUSH hb = DlssNrBrush(RGB(0xF2, 0xF2, 0xF2));
    HBRUSH oldB = (HBRUSH)SelectObject(dc, hb);
    HPEN   oldP = (HPEN)SelectObject(dc, GetStockObject(NULL_PEN));
    Ellipse(dc, splitX - hs, (dst.top + dst.bottom) / 2 - hs,
                splitX + hs, (dst.top + dst.bottom) / 2 + hs);
    SelectObject(dc, oldB);
    SelectObject(dc, oldP);

    // Labels: "chip" boxes sized from the actual text, so nothing is ever clipped and
    // both follow the panel language (they are drawn per paint, not baked in).
    SelectObject(dc, DlssNrCompareFont());
    const wchar_t* lz = T(L"原始", L"Original");
    const wchar_t* rz = L"DLSSNR";
    const int chipH = DlssNrGetMetrics().lineH + DlssNrScaled(6);
    const int chipPad = DlssNrScaled(12);
    RECT lb = { dst.left + DlssNrScaled(10), dst.top + DlssNrScaled(8), 0, 0 };
    lb.right = lb.left + DlssNrExtent(dc, lz) + 2 * chipPad;
    lb.bottom = lb.top + chipH;
    RECT rb = { dst.right - DlssNrScaled(10), dst.top + DlssNrScaled(8), 0, 0 };
    rb.left = rb.right - (DlssNrExtent(dc, rz) + 2 * chipPad);
    rb.bottom = rb.top + chipH;
    HBRUSH chip = DlssNrBrush(RGB(0x12, 0x12, 0x12));
    FillRect(dc, &lb, chip);
    FillRect(dc, &rb, chip);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, RGB(0xFF, 0xFF, 0xFF));
    DrawTextW(dc, lz, -1, &lb, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    DrawTextW(dc, rz, -1, &rb, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);

    // One blit for the whole window: no flicker.
    BitBlt(wdc, 0, 0, rc.right, rc.bottom, dc, 0, 0, SRCCOPY);
    if (oldBmp) SelectObject(dc, oldBmp);
    if (bmp) DeleteObject(bmp);
    DeleteDC(dc);
    EndPaint(hwnd, &ps);
}

static LRESULT CALLBACK DlssNrCompareProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CREATE:
        g_cmp.hwnd = hwnd;
        g_cmp.split = 50;
        g_cmp.seq = -1;
        SetTimer(hwnd, 1, 120, nullptr);
        return 0;
    case WM_TIMER: {
        const LONG sq = DlssNrPreviewSeq();
        if (sq != g_cmp.seq) { g_cmp.seq = sq; InvalidateRect(hwnd, nullptr, FALSE); }
        return 0;
    }
    case WM_PAINT:
        DlssNrComparePaint(hwnd);
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_LBUTTONDOWN:
        SetCapture(hwnd);
        g_cmp.dragging = true;
        DlssNrCompareSetSplit(hwnd, ((int)(short)LOWORD(lp)));
        return 0;
    case WM_MOUSEMOVE:
        if (g_cmp.dragging) DlssNrCompareSetSplit(hwnd, ((int)(short)LOWORD(lp)));
        else SetCursor(LoadCursorW(nullptr, (LPCWSTR)IDC_SIZEWE));
        return 0;
    case WM_LBUTTONUP:
        if (g_cmp.dragging) { g_cmp.dragging = false; ReleaseCapture(); }
        return 0;
    case WM_SETCURSOR:
        if (LOWORD(lp) == HTCLIENT) { SetCursor(LoadCursorW(nullptr, (LPCWSTR)IDC_SIZEWE)); return TRUE; }
        break;
    case WM_KEYDOWN:
        if (wp == VK_LEFT || wp == VK_RIGHT) {
            g_cmp.split += (wp == VK_LEFT) ? -2 : 2;
            if (g_cmp.split < 2) g_cmp.split = 2;
            if (g_cmp.split > 98) g_cmp.split = 98;
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }
        break;
    case WM_CLOSE: {
        // Closing the window leaves the compare feature: stop feeding the buffer and
        // put the panel back on its first page.
        HWND owner = GetWindow(hwnd, GW_OWNER);
        DlssNrPreviewSetWant(false);
        ShowWindow(hwnd, SW_HIDE);
        if (owner) {
            DlssNrSettings* s = (DlssNrSettings*)GetWindowLongPtrW(owner, GWLP_USERDATA);
            if (s && s->page == 2) {
                s->page = 0;
                DlssNrLayout(s);
                for (int i = 0; i < 3; ++i) InvalidateRect(s->tab[i], nullptr, FALSE);
            }
        }
        return 0;
    }
    case WM_DESTROY:
        KillTimer(hwnd, 1);
        g_cmp.hwnd = nullptr;
        return 0;
    default: break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static const wchar_t* DlssNrCompareTitle() {
    return T(L"DLSSNR 画面对比 —— 左：原始　右：DLSSNR　（拖动分隔线；面板里改参数即时生效）",
             L"DLSSNR A/B compare - left: original, right: DLSSNR (drag the divider; edits apply live)");
}

// Opens / hides the compare window (called from the panel's 对比 tab).
static void DlssNrCompareShow(DlssNrSettings* s, bool on) {
    if (!s) return;
    if (!on) {
        DlssNrPreviewSetWant(false);
        if (s->compareWnd) ShowWindow(s->compareWnd, SW_HIDE);
        return;
    }
    if (!s->compareWnd) {
        static bool cmpClassReady = false;
        if (!cmpClassReady) {
            WNDCLASSEXW wc = { sizeof(wc) };
            wc.lpfnWndProc   = DlssNrCompareProc;
            wc.hInstance     = g_hModule;
            wc.hCursor       = LoadCursorW(nullptr, (LPCWSTR)IDC_ARROW);
            wc.hbrBackground = DlssNrBrush(kClrBg);
            wc.lpszClassName = L"DlssNrCompareWnd";
            if (!RegisterClassExW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
                return;
            cmpClassReady = true;
        }
        s->compareWnd = CreateWindowExW(
            0, L"DlssNrCompareWnd", DlssNrCompareTitle(),
            WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
            DlssNrScaled(1000), DlssNrScaled(520), s->hwnd, nullptr, g_hModule, s);
        if (!s->compareWnd) return;
    }
    DlssNrPreviewSetWant(true);
    ShowWindow(s->compareWnd, SW_SHOW);
    SetForegroundWindow(s->compareWnd);
    InvalidateRect(s->compareWnd, nullptr, FALSE);
}

// ---- window ----------------------------------------------------------------
static LRESULT CALLBACK DlssNrProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    DlssNrSettings* s = (DlssNrSettings*)GetWindowLongPtrW(hwnd, GWLP_USERDATA);

    switch (msg) {
    case WM_CREATE: {
        s = (DlssNrSettings*)((CREATESTRUCTW*)lp)->lpCreateParams;
        s->hwnd = hwnd;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)s);
        DlssNrLoadLang();

        NONCLIENTMETRICSW ncm = { sizeof(NONCLIENTMETRICSW) };
        if (SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0)) {
            s->font = CreateFontIndirectW(&ncm.lfMessageFont);
            LOGFONTW b = ncm.lfMessageFont;
            b.lfWeight = FW_BOLD;
            s->fontBold = CreateFontIndirectW(&b);
            LOGFONTW big = ncm.lfMessageFont;
            big.lfHeight = (LONG)(ncm.lfMessageFont.lfHeight * 1.5);
            big.lfWeight = FW_BOLD;
            s->fontBig = CreateFontIndirectW(&big);
            LOGFONTW sm = ncm.lfMessageFont;
            sm.lfHeight = (LONG)(ncm.lfMessageFont.lfHeight * 0.95);
            s->fontSmall = CreateFontIndirectW(&sm);
        } else {
            s->font = s->fontBold = s->fontBig = s->fontSmall =
                (HFONT)GetStockObject(DEFAULT_GUI_FONT);
        }

        for (int i = 0; i < 3; ++i) {
            s->tab[i] = CreateWindowExW(0, L"BUTTON", L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
                                        0, 0, 0, 0, hwnd,
                                        (HMENU)(INT_PTR)(i == 2 ? IDC_DLS_TAB2 : IDC_DLS_TAB0 + i), g_hModule, nullptr);
        }
        for (int i = 0; i < 2; ++i) {
            s->lang[i] = CreateWindowExW(0, L"BUTTON", L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
                                         0, 0, 0, 0, hwnd,
                                         (HMENU)(INT_PTR)(IDC_DLS_LANG0 + i), g_hModule, nullptr);
        }
        for (int i = 0; i < kDlssNrStyleCount; ++i) {
            s->styleBtn[i] = CreateWindowExW(0, L"BUTTON", L"",
                                             WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
                                             0, 0, 0, 0, hwnd,
                                             (HMENU)(INT_PTR)(IDC_DLS_STYLE0 + i), g_hModule, nullptr);
        }
        // The model row: which network inside nvngx_dlssnr.dll. All four segments
        // are created visible; DlssNrSyncModelButtons() decides which of them the
        // loaded DLL can serve and draws the rest inert.
        s->lblModel = CreateWindowExW(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE | SS_LEFT,
                                      0, 0, 0, 0, hwnd, (HMENU)(INT_PTR)IDC_DLS_MODELLBL,
                                      g_hModule, nullptr);
        for (int i = 0; i < kDlssNrModelBtnCount; ++i) {
            s->modelBtn[i] = CreateWindowExW(0, L"BUTTON", L"",
                                             WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
                                             0, 0, 0, 0, hwnd,
                                             (HMENU)(INT_PTR)(IDC_DLS_MODEL0 + i), g_hModule, nullptr);
        }
        // Layer count stepper + the 1..N strip. The strip buttons are created
        // WITHOUT WS_VISIBLE: only the first `count` of them may ever show, and
        // DlssNrSyncLayerStrip() owns that decision (page AND count).
        s->lblLayerName = CreateWindowExW(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE | SS_LEFT,
                                          0, 0, 0, 0, hwnd, (HMENU)(INT_PTR)IDC_DLS_LAYERNAME, g_hModule, nullptr);
        s->btnLayerMinus = CreateWindowExW(0, L"BUTTON", L"",
                                           WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
                                           0, 0, 0, 0, hwnd,
                                           (HMENU)(INT_PTR)IDC_DLS_LCOUNT_MINUS, g_hModule, nullptr);
        s->lblLayerCount = CreateWindowExW(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE | SS_CENTER,
                                           0, 0, 0, 0, hwnd, (HMENU)(INT_PTR)IDC_DLS_LAYERCOUNT, g_hModule, nullptr);
        s->btnLayerPlus = CreateWindowExW(0, L"BUTTON", L"",
                                          WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
                                          0, 0, 0, 0, hwnd,
                                          (HMENU)(INT_PTR)IDC_DLS_LCOUNT_PLUS, g_hModule, nullptr);
        for (int i = 0; i < (int)dlssnr::kMaxLayers; ++i) {
            s->layerBtn[i] = CreateWindowExW(0, L"BUTTON", L"",
                                             WS_CHILD | WS_TABSTOP | BS_OWNERDRAW,
                                             0, 0, 0, 0, hwnd,
                                             (HMENU)(INT_PTR)(IDC_DLS_LAYER0 + i), g_hModule, nullptr);
        }
        s->lblLayerNote = CreateWindowExW(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE | SS_LEFT,
                                          0, 0, 0, 0, hwnd, (HMENU)(INT_PTR)IDC_DLS_LAYERNOTE, g_hModule, nullptr);
        for (int i = 0; i < kDlssNrSliderCount; ++i) {
            const int base = IDC_DLS_LBL0 + i * 10;
            s->lbl[i] = CreateWindowExW(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE | SS_LEFT,
                                        0, 0, 0, 0, hwnd, (HMENU)(INT_PTR)(base), g_hModule, nullptr);
            s->trk[i] = CreateWindowExW(0, TRACKBAR_CLASSW, L"",
                                        WS_CHILD | WS_VISIBLE | WS_TABSTOP | TBS_HORZ | TBS_NOTICKS,
                                        0, 0, 0, 0, hwnd, (HMENU)(INT_PTR)(base + 1), g_hModule, nullptr);
            SendMessageW(s->trk[i], TBM_SETRANGE, TRUE, MAKELPARAM(0, kDlssNrSliderMax[i]));
            SendMessageW(s->trk[i], TBM_SETPOS, TRUE, DlssNrSliderDefault(i));
            SetWindowSubclass(s->trk[i], DlssNrSliderProc, 0, 0);
            s->val[i] = CreateWindowExW(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE | SS_RIGHT,
                                        0, 0, 0, 0, hwnd, (HMENU)(INT_PTR)(base + 2), g_hModule, nullptr);
            s->rst[i] = CreateWindowExW(0, L"BUTTON", L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
                                        0, 0, 0, 0, hwnd, (HMENU)(INT_PTR)(base + 3), g_hModule, nullptr);
        }
        // The reduced-resolution row: same columns as a slider row, but its own
        // range (25..100%) and its own handler -- changing it re-creates the
        // engine's NGX session instead of pushing a parameter.
        s->lblProc = CreateWindowExW(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE | SS_LEFT,
                                     0, 0, 0, 0, hwnd, (HMENU)(INT_PTR)IDC_DLS_PROCLBL, g_hModule, nullptr);
        s->trkProc = CreateWindowExW(0, TRACKBAR_CLASSW, L"",
                                     WS_CHILD | WS_VISIBLE | WS_TABSTOP | TBS_HORZ | TBS_NOTICKS,
                                     0, 0, 0, 0, hwnd, (HMENU)(INT_PTR)IDC_DLS_PROCTRK, g_hModule, nullptr);
        SendMessageW(s->trkProc, TBM_SETRANGE, TRUE, MAKELPARAM(25, 100));
        SendMessageW(s->trkProc, TBM_SETLINESIZE, 0, 5);
        SendMessageW(s->trkProc, TBM_SETPAGESIZE, 0, 5);
        SetWindowSubclass(s->trkProc, DlssNrSliderProc, 0, 0);
        s->valProc = CreateWindowExW(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE | SS_RIGHT,
                                     0, 0, 0, 0, hwnd, (HMENU)(INT_PTR)IDC_DLS_PROCVAL, g_hModule, nullptr);
        // Extrapolation: a gain, so 100 % is the floor and means off.
        s->lblExtrap = CreateWindowExW(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE | SS_LEFT,
                                       0, 0, 0, 0, hwnd, (HMENU)(INT_PTR)IDC_DLS_EXTRAPLBL, g_hModule, nullptr);
        s->trkExtrap = CreateWindowExW(0, TRACKBAR_CLASSW, L"",
                                       WS_CHILD | WS_VISIBLE | WS_TABSTOP | TBS_HORZ | TBS_NOTICKS,
                                       0, 0, 0, 0, hwnd, (HMENU)(INT_PTR)IDC_DLS_EXTRAPTRK, g_hModule, nullptr);
        SendMessageW(s->trkExtrap, TBM_SETRANGE, TRUE, MAKELPARAM(100, 400));
        SendMessageW(s->trkExtrap, TBM_SETLINESIZE, 0, 10);
        SendMessageW(s->trkExtrap, TBM_SETPAGESIZE, 0, 10);
        SetWindowSubclass(s->trkExtrap, DlssNrSliderProc, 0, 0);
        s->valExtrap = CreateWindowExW(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE | SS_RIGHT,
                                       0, 0, 0, 0, hwnd, (HMENU)(INT_PTR)IDC_DLS_EXTRAPVAL, g_hModule, nullptr);
        // Reset buttons for the two global rows, same owner-draw shape as rst[i].
        s->rstProc = CreateWindowExW(0, L"BUTTON", L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
                                     0, 0, 0, 0, hwnd, (HMENU)(INT_PTR)IDC_DLS_PROCRST, g_hModule, nullptr);
        s->rstExtrap = CreateWindowExW(0, L"BUTTON", L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
                                       0, 0, 0, 0, hwnd, (HMENU)(INT_PTR)IDC_DLS_EXTRAPRST, g_hModule, nullptr);
        s->chkEnable = CreateWindowExW(0, L"BUTTON", L"",
                                       WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX,
                                       0, 0, 0, 0, hwnd, (HMENU)(INT_PTR)IDC_DLS_ENABLE, g_hModule, nullptr);
        s->chkAutoMask = CreateWindowExW(0, L"BUTTON", L"",
                                         WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX,
                                         0, 0, 0, 0, hwnd, (HMENU)(INT_PTR)IDC_DLS_AUTOMASK, g_hModule, nullptr);
        s->chkDrop = CreateWindowExW(0, L"BUTTON", L"",
                                     WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX,
                                     0, 0, 0, 0, hwnd, (HMENU)(INT_PTR)IDC_DLS_DROP, g_hModule, nullptr);
        s->chkProc = CreateWindowExW(0, L"BUTTON", L"",
                                     WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX,
                                     0, 0, 0, 0, hwnd, (HMENU)(INT_PTR)IDC_DLS_PROCCHK, g_hModule, nullptr);
        s->chkAutoshow = CreateWindowExW(0, L"BUTTON", L"",
                                         WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX,
                                         0, 0, 0, 0, hwnd, (HMENU)(INT_PTR)IDC_DLS_AUTOSHOW, g_hModule, nullptr);
        s->btnResetAll = CreateWindowExW(0, L"BUTTON", L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
                                         0, 0, 0, 0, hwnd, (HMENU)(INT_PTR)IDC_DLS_RESETALL, g_hModule, nullptr);
        s->btnRecheck = CreateWindowExW(0, L"BUTTON", L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
                                        0, 0, 0, 0, hwnd, (HMENU)(INT_PTR)IDC_DLS_RECHECK, g_hModule, nullptr);
        s->btnLog = CreateWindowExW(0, L"BUTTON", L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
                                    0, 0, 0, 0, hwnd, (HMENU)(INT_PTR)IDC_DLS_LOG, g_hModule, nullptr);

        // The borderless header's close button exists only in the standalone
        // panel; the property-page copy lives inside the player's dialog.
        if (s->standalone) {
            s->btnClose = CreateWindowExW(0, L"BUTTON", L"",
                                          WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
                                          0, 0, 0, 0, hwnd, (HMENU)(INT_PTR)IDC_DLS_CLOSE,
                                          g_hModule, nullptr);
        }

        s->lblTitle = CreateWindowExW(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE | SS_LEFT,
                                      0, 0, 0, 0, hwnd, (HMENU)(INT_PTR)IDC_DLS_TITLE, g_hModule, nullptr);
        s->lblSub   = CreateWindowExW(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE | SS_LEFT | SS_ENDELLIPSIS,
                                      0, 0, 0, 0, hwnd, (HMENU)(INT_PTR)IDC_DLS_SUB, g_hModule, nullptr);
        s->lblStats = CreateWindowExW(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE | SS_LEFT,
                                      0, 0, 0, 0, hwnd, (HMENU)(INT_PTR)IDC_DLS_STATS, g_hModule, nullptr);
        s->lblGeom  = CreateWindowExW(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE | SS_LEFT,
                                      0, 0, 0, 0, hwnd, (HMENU)(INT_PTR)IDC_DLS_GEOM, g_hModule, nullptr);
        s->lblLayers = CreateWindowExW(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE | SS_LEFT,
                                       0, 0, 0, 0, hwnd, (HMENU)(INT_PTR)IDC_DLS_LAYERS, g_hModule, nullptr);
        s->lblModelLine = CreateWindowExW(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE | SS_LEFT | SS_ENDELLIPSIS,
                                          0, 0, 0, 0, hwnd, (HMENU)(INT_PTR)IDC_DLS_MODELLINE,
                                          g_hModule, nullptr);
        s->lblHint  = CreateWindowExW(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE | SS_LEFT,
                                      0, 0, 0, 0, hwnd, (HMENU)(INT_PTR)IDC_DLS_HINT, g_hModule, nullptr);

        HWND all[] = { s->tab[0], s->tab[1], s->lang[0], s->lang[1],
                       s->styleBtn[0], s->styleBtn[1], s->styleBtn[2], s->styleBtn[3],
                       s->lbl[0], s->trk[0], s->val[0], s->rst[0],
                       s->lbl[1], s->trk[1], s->val[1], s->rst[1],
                       s->lbl[2], s->trk[2], s->val[2], s->rst[2],
                       s->lbl[3], s->trk[3], s->val[3], s->rst[3],
                       s->lblLayerName, s->lblLayerCount, s->lblLayerNote,
                       s->btnLayerMinus, s->btnLayerPlus,
                       s->layerBtn[0], s->layerBtn[1], s->layerBtn[2], s->layerBtn[3],
                       s->layerBtn[4], s->layerBtn[5], s->layerBtn[6], s->layerBtn[7],
                       s->chkEnable, s->chkAutoMask, s->chkDrop, s->chkProc, s->chkAutoshow,
                       s->lblProc, s->trkProc, s->valProc,
        s->lblExtrap, s->trkExtrap, s->valExtrap,
        s->rstProc, s->rstExtrap,
                       s->lblModel, s->modelBtn[0], s->modelBtn[1], s->modelBtn[2], s->modelBtn[3],
                       s->btnResetAll, s->btnRecheck, s->btnLog,
                       s->lblTitle, s->lblSub, s->lblStats, s->lblGeom, s->lblLayers, s->lblHint,
                       s->lblModelLine };
        for (HWND h : all) if (h) SendMessageW(h, WM_SETFONT, (WPARAM)s->font, TRUE);
        // Bold chrome, small type for the readouts, telemetry and hint.
        for (int i = 0; i < 2; ++i) {
            SendMessageW(s->tab[i], WM_SETFONT, (WPARAM)s->fontBold, TRUE);
            SendMessageW(s->lang[i], WM_SETFONT, (WPARAM)s->fontBold, TRUE);
        }
        for (int i = 0; i < kDlssNrStyleCount; ++i)
            SendMessageW(s->styleBtn[i], WM_SETFONT, (WPARAM)s->fontBold, TRUE);
        // The model row is chrome like the style row: caption and segments bold.
        SendMessageW(s->lblModel, WM_SETFONT, (WPARAM)s->fontBold, TRUE);
        for (int i = 0; i < kDlssNrModelBtnCount; ++i)
            SendMessageW(s->modelBtn[i], WM_SETFONT, (WPARAM)s->fontBold, TRUE);
        for (int i = 0; i < kDlssNrSliderCount; ++i)
            SendMessageW(s->lbl[i], WM_SETFONT, (WPARAM)s->fontBold, TRUE);
        // the processing-resolution row is a slider row too, so its label is bold
        SendMessageW(s->lblProc, WM_SETFONT, (WPARAM)s->fontBold, TRUE);
        SendMessageW(s->lblExtrap, WM_SETFONT, (WPARAM)s->fontBold, TRUE);
        // The layer row is chrome: its caption, the N readout and the buttons are
        // all bold like the other labels/segments.
        SendMessageW(s->lblLayerName, WM_SETFONT, (WPARAM)s->fontBold, TRUE);
        SendMessageW(s->lblLayerCount, WM_SETFONT, (WPARAM)s->fontBold, TRUE);
        SendMessageW(s->btnLayerMinus, WM_SETFONT, (WPARAM)s->fontBold, TRUE);
        SendMessageW(s->btnLayerPlus,  WM_SETFONT, (WPARAM)s->fontBold, TRUE);
        for (int i = 0; i < (int)dlssnr::kMaxLayers; ++i)
            SendMessageW(s->layerBtn[i], WM_SETFONT, (WPARAM)s->fontBold, TRUE);
        SendMessageW(s->lblTitle, WM_SETFONT, (WPARAM)s->fontBig, TRUE);
        // Plain array, not an initializer_list range-for: this header must
        // compile in a translation unit that does not include <initializer_list>.
        HWND smallType[] = { s->lblSub, s->lblStats, s->lblGeom, s->lblHint,
                             s->lblLayerNote, s->lblLayers,
                             s->val[0], s->val[1], s->val[2], s->val[3], s->valProc,
                             s->valExtrap };
        for (HWND h : smallType) SendMessageW(h, WM_SETFONT, (WPARAM)s->fontSmall, TRUE);

        DlssNrAttachHover(s->tab[0]);   DlssNrAttachHover(s->tab[1]);
        DlssNrAttachHover(s->lang[0]);  DlssNrAttachHover(s->lang[1]);
        for (int i = 0; i < kDlssNrStyleCount; ++i) DlssNrAttachHover(s->styleBtn[i]);
        DlssNrAttachHover(s->btnLayerMinus);
        DlssNrAttachHover(s->btnLayerPlus);
        for (int i = 0; i < (int)dlssnr::kMaxLayers; ++i) DlssNrAttachHover(s->layerBtn[i]);
        for (int i = 0; i < kDlssNrSliderCount; ++i) DlssNrAttachHover(s->rst[i]);
        DlssNrAttachHover(s->rstProc);
        DlssNrAttachHover(s->rstExtrap);
        DlssNrAttachHover(s->btnResetAll);
        DlssNrAttachHover(s->btnRecheck);
        DlssNrAttachHover(s->btnLog);
        DlssNrAttachHover(s->btnClose);
        if (s->btnClose) SendMessageW(s->btnClose, WM_SETFONT, (WPARAM)s->fontBold, TRUE);
        // No AttachHover() for the checkboxes: DlssNrCheckProc already handles
        // hover (and painting), and both would claim subclass id 1.
        // Checkboxes are real checkboxes that paint themselves (see DlssNrCheckProc).
        SetWindowSubclass(s->chkEnable,   DlssNrCheckProc, 1, 0);
        SetWindowSubclass(s->chkAutoMask, DlssNrCheckProc, 1, 0);
        SetWindowSubclass(s->chkDrop,     DlssNrCheckProc, 1, 0);
        SetWindowSubclass(s->chkProc,     DlssNrCheckProc, 1, 0);
        SetWindowSubclass(s->chkAutoshow, DlssNrCheckProc, 1, 0);

        s->page = 0;
        s->shownStyle = Engine::Instance().OptStyle();
        DlssNrRetext(hwnd);
        DlssNrRefreshControls(hwnd);
        DlssNrRefreshStatus(hwnd);
        DlssNrLayout(s);
        SetTimer(hwnd, 1, 500, nullptr);
        return 0;
    }

    case WM_ERASEBKGND:
        return 1;                       // WM_PAINT paints the whole client area

    case WM_PAINT: {
        PAINTSTRUCT ps = {};
        HDC dc = BeginPaint(hwnd, &ps);
        RECT rc = {};
        GetClientRect(hwnd, &rc);
        DlssNrFill(dc, rc, kClrBg);
        if (s) {
            const DlssNrMetrics& m = DlssNrGetMetrics();
            const int headH = s->standalone ? m.headerH : 0;
            const int hair = DlssNrMax2(1, m.grooveH / 2);

            // Own title bar: the standalone panel is borderless, so this is the
            // only thing that identifies and drags the window.
            if (headH > 0) {
                RECT head = { 0, 0, rc.right, headH };
                DlssNrFill(dc, head, kClrStrip);
                RECT headLine = { 0, headH - hair, rc.right, headH };
                DlssNrFill(dc, headLine, kClrEdge);
                RECT t = { m.margin, 0, rc.right - m.closeW - 2 * m.margin, headH };
                SetBkMode(dc, TRANSPARENT);
                SetTextColor(dc, kClrTextDim);
                HGDIOBJ old = SelectObject(dc, s->fontBold);
                DrawTextW(dc, T(L"DLSSNR 控制面板", L"DLSSNR Control Panel"), -1, &t,
                          DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
                SelectObject(dc, old);
            }

            // Tab strip band + accent underline.
            RECT strip = { 0, headH, rc.right, headH + m.gap + m.tabH + m.gap };
            if (strip.bottom > rc.bottom) strip.bottom = rc.bottom;
            DlssNrFill(dc, strip, kClrStrip);
            RECT sep = { 0, strip.bottom - hair, rc.right, strip.bottom };
            DlssNrFill(dc, sep, kClrAccent);

            if (s->standalone) {
                // Borderless: draw our own outline instead of the classic frame.
                RECT b = rc;
                DlssNrFrame(dc, b, kClrEdge);
            }
        }
        EndPaint(hwnd, &ps);
        return 0;
    }

    case WM_NCHITTEST: {
        if (!s || !s->standalone) break;
        POINT pt = { (short)LOWORD(lp), (short)HIWORD(lp) };
        ScreenToClient(hwnd, &pt);
        if (pt.y >= 0 && pt.y < DlssNrGetMetrics().headerH) return HTCAPTION;
        break;
    }

    case WM_DRAWITEM: {
        if (!s) break;
        const DRAWITEMSTRUCT* di = (const DRAWITEMSTRUCT*)lp;
        const int id = (int)di->CtlID;
        bool active = false;
        switch (id) {
        case IDC_DLS_TAB0:      active = (s->page == 0); break;
        case IDC_DLS_TAB0 + 1:  active = (s->page == 1); break;
        case IDC_DLS_TAB2:      active = (s->page == 2); break;
        case IDC_DLS_LANG0:     active = (g_uiLang == 0); break;
        case IDC_DLS_LANG0 + 1: active = (g_uiLang == 1); break;
        default:
            if (id >= IDC_DLS_LAYER0 && id < IDC_DLS_LAYER0 + (int)dlssnr::kMaxLayers)
                active = (Engine::Instance().OptSelLayer() == id - IDC_DLS_LAYER0);
            else if (id >= IDC_DLS_STYLE0 && id < IDC_DLS_STYLE0 + kDlssNrStyleCount)
                active = (Engine::Instance().OptStyle() == id - IDC_DLS_STYLE0);
            else if (id >= IDC_DLS_MODEL0 && id < IDC_DLS_MODEL0 + kDlssNrModelBtnCount)
                active = DlssNrModelSegActive(id - IDC_DLS_MODEL0,
                                              Engine::Instance().OptModel());
            break;
        }
        DlssNrDrawButton(di, DlssNrButtonLabel(id), active);
        return TRUE;
    }

    case WM_CTLCOLORSTATIC: {
        HDC dc = (HDC)wp;
        SetBkMode(dc, TRANSPARENT);
        COLORREF col = kClrText;
        if (s) {
            if ((HWND)lp == s->lblTitle)          col = g_statusColour;
            else if ((HWND)lp == s->lblHint ||
                     (HWND)lp == s->lblLayerNote) col = kClrTextDim;
        }
        SetTextColor(dc, col);
        return (LRESULT)DlssNrBrush(kClrBg);
    }

    case WM_SIZE:
        if (s) DlssNrLayout(s);
        return 0;

    case WM_TIMER: {
        if (!s) return 0;
        Engine& e = Engine::Instance();
        DlssNrSetChecked(s->chkEnable, e.OptEnabled());
        DlssNrSetChecked(s->chkAutoMask, e.OptAutoMask());
        DlssNrSetChecked(s->chkDrop, e.OptDropFrames());   // the tray can change it
        DlssNrSetChecked(s->chkProc, e.OptProcEnabled());
        DlssNrSetChecked(s->chkAutoshow, DlssNrAutoshow());
        if (s->shownStyle != e.OptStyle()) {       // the tray menu can change it too
            s->shownStyle = e.OptStyle();
            for (int i = 0; i < kDlssNrStyleCount; ++i)
                InvalidateRect(s->styleBtn[i], nullptr, FALSE);
        }
        if (s->shownModel != e.OptModel())         // ...and the model selection
            DlssNrSyncModelButtons(s);             // sets shownModel and repaints
        // The tray (or another copy of the panel) can change the chain too. A
        // count change copies the last layer's values into the new slots, so a
        // partial repaint is not enough: reload every control from the engine.
        if (s->shownLayerCount != e.OptLayerCount() || s->shownSelLayer != e.OptSelLayer())
            DlssNrRefreshControls(hwnd);
        DlssNrRefreshStatus(hwnd);
        return 0;
    }

    case WM_HSCROLL: {
        if (!s) break;
        HWND from = (HWND)lp;
        if (from == s->trkProc) {
            // Readout live, rebuild only when the drag ends: every change
            // re-creates the NGX session, which briefly passes frames through.
            const int pct = (int)SendMessageW(from, TBM_GETPOS, 0, 0);
            wchar_t t[16];
            _snwprintf_s(t, _countof(t), _TRUNCATE, L"%d%%", pct);
            SetWindowTextW(s->valProc, t);
            InvalidateRect(from, nullptr, FALSE);
            if (LOWORD(wp) == TB_ENDTRACK)
                DlssNrApplyProcScale(DlssNrChecked(s->chkProc), pct, true);
            return 0;
        }
        if (from == s->trkExtrap) {
            // Live readout on every tick, applied on TB_ENDTRACK only: the gain
            // is a shader constant, so an end-track apply is a picture that
            // changes once per drag instead of strobing through every step.
            const int gain = (int)SendMessageW(from, TBM_GETPOS, 0, 0);
            wchar_t t[16];
            if (gain == 100) _snwprintf_s(t, _countof(t), _TRUNCATE, L"%s", T(L"关", L"off"));
            else             _snwprintf_s(t, _countof(t), _TRUNCATE, L"%d%%", gain);
            SetWindowTextW(s->valExtrap, t);
            InvalidateRect(from, nullptr, FALSE);
            if (LOWORD(wp) == TB_ENDTRACK) DlssNrApplyExtrap(gain, true);
            return 0;
        }
        int idx = -1;
        for (int i = 0; i < kDlssNrSliderCount; ++i) if (from == s->trk[i]) idx = i;
        if (idx < 0) break;
        const int pct = (int)SendMessageW(from, TBM_GETPOS, 0, 0);
        wchar_t t[16];
        _snwprintf_s(t, _countof(t), _TRUNCATE, L"%d%%", pct);
        SetWindowTextW(s->val[idx], t);
        InvalidateRect(from, nullptr, FALSE);
        InvalidateRect(s->rst[idx], nullptr, FALSE);
        DlssNrPush(hwnd, LOWORD(wp) == TB_ENDTRACK);
        return 0;
    }

    case WM_COMMAND: {
        if (!s) break;
        const int id = LOWORD(wp), code = HIWORD(wp);
        if (code != BN_CLICKED) break;

        if (id == IDC_DLS_TAB0 || id == IDC_DLS_TAB0 + 1 || id == IDC_DLS_TAB2) {
            s->page = (id == IDC_DLS_TAB0) ? 0 : ((id == IDC_DLS_TAB0 + 1) ? 1 : 2);
            DlssNrLayout(s);
            for (int i = 0; i < 3; ++i) InvalidateRect(s->tab[i], nullptr, FALSE);
            // Page 2 IS the comparison window: opening the tab opens it, leaving the
            // tab hides it. It is the only place a paused parameter change can be seen
            // (see RefreshIfNeeded in the filter).
            if (s->page == 2) DlssNrCompareShow(s, true);
            return 0;
        }
        if (id == IDC_DLS_LANG0 || id == IDC_DLS_LANG0 + 1) {
            g_uiLang = (id == IDC_DLS_LANG0) ? 0 : 1;
            wchar_t ini[MAX_PATH];
            DlssNrIniPath(ini, MAX_PATH);
            WritePrivateProfileStringW(L"DLSSNR", L"lang", g_uiLang == 1 ? L"en" : L"zh", ini);
            DlssNrRetext(hwnd);
            DlssNrRefreshStatus(hwnd);
            DlssNrLayout(s);
            return 0;
        }
        if (id == IDC_DLS_LCOUNT_MINUS || id == IDC_DLS_LCOUNT_PLUS) {
            const int cur = Engine::Instance().OptLayerCount();
            DlssNrSetLayerCountFromUi(hwnd, cur + (id == IDC_DLS_LCOUNT_PLUS ? 1 : -1));
            return 0;
        }
        if (id >= IDC_DLS_LAYER0 && id < IDC_DLS_LAYER0 + (int)dlssnr::kMaxLayers) {
            // Selection only decides which layer the controls below edit. It is
            // NOT a parameter change: pushing the on-screen values now would
            // overwrite the layer just selected with the old layer's numbers, and
            // there is nothing to write to the ini either. Reload from the engine.
            Engine::Instance().SetSelLayer(id - IDC_DLS_LAYER0);
            DlssNrRefreshControls(hwnd);
            return 0;
        }
        if (id >= IDC_DLS_STYLE0 && id < IDC_DLS_STYLE0 + kDlssNrStyleCount) {
            int pct[kDlssNrSliderCount];
            for (int i = 0; i < kDlssNrSliderCount; ++i)
                pct[i] = (int)SendMessageW(s->trk[i], TBM_GETPOS, 0, 0);
            DlssNrApply(DlssNrChecked(s->chkEnable), id - IDC_DLS_STYLE0,
                        pct[0], pct[1], pct[2], pct[3],
                        DlssNrChecked(s->chkAutoMask), true);
            s->shownStyle = id - IDC_DLS_STYLE0;
            for (int i = 0; i < kDlssNrStyleCount; ++i)
                InvalidateRect(s->styleBtn[i], nullptr, FALSE);
            if (s->onChanged) s->onChanged(s->onChangedCtx);
            HWND par = GetParent(hwnd);
            if (par) PostMessageW(par, DLS_N_CHANGED, 0, 0);
            return 0;
        }
        if (id >= IDC_DLS_MODEL0 && id < IDC_DLS_MODEL0 + kDlssNrModelBtnCount) {
            // A model change re-creates the session, so this behaves like the
            // processing-resolution row and not like the sliders: apply, persist,
            // and let the worker rebuild (frames pass through for that moment).
            // The raw-id segment advances to the next id no fixed segment covers.
            const int seg = id - IDC_DLS_MODEL0;
            const int cur = Engine::Instance().OptModel();
            const int preset = (seg == kDlssNrModelBtnCustom)
                             ? DlssNrModelCustomNext(cur)
                             : DlssNrModelSegmentPreset(seg, cur);
            DlssNrApplyModel(preset, true);
            DlssNrSyncModelButtons(s);
            return 0;
        }
        if (id == IDC_DLS_PROCRST) {
            // Documented default, not "off": the panel restores the value the
            // switch was designed around, and leaves the switch itself alone --
            // resetting a number must not silently enable a feature.
            SendMessageW(s->trkProc, TBM_SETPOS, TRUE, 50);
            DlssNrApplyProcScale(DlssNrChecked(s->chkProc), 50, true);
            DlssNrRefreshControls(hwnd);
            return 0;
        }
        if (id == IDC_DLS_EXTRAPRST) {
            SendMessageW(s->trkExtrap, TBM_SETPOS, TRUE, 100);
            DlssNrApplyExtrap(100, true);          // 100 = off
            DlssNrRefreshControls(hwnd);
            return 0;
        }
        if (id >= IDC_DLS_RST0 && id < IDC_DLS_RST0 + kDlssNrSliderCount * 10 &&
            (id - IDC_DLS_RST0) % 10 == 0) {
            const int i = (id - IDC_DLS_RST0) / 10;
            SendMessageW(s->trk[i], TBM_SETPOS, TRUE, DlssNrSliderDefault(i));
            InvalidateRect(s->trk[i], nullptr, FALSE);
            DlssNrPush(hwnd, true);
            return 0;
        }
        if (id == IDC_DLS_ENABLE || id == IDC_DLS_AUTOMASK) {
            DlssNrPush(hwnd, true);
            return 0;
        }
        if (id == IDC_DLS_DROP) {
            DlssNrApplyDrop(DlssNrChecked(s->chkDrop), true);
            return 0;
        }
        if (id == IDC_DLS_PROCCHK) {
            const int pct = (int)SendMessageW(s->trkProc, TBM_GETPOS, 0, 0);
            DlssNrApplyProcScale(DlssNrChecked(s->chkProc), pct, true);
            DlssNrRefreshControls(hwnd);      // greys the slider out when it is off
            return 0;
        }
        if (id == IDC_DLS_AUTOSHOW) {
            const bool now = DlssNrChecked(s->chkAutoshow);
            DlssNrIniWriteInt(L"autoshow", now ? 1 : 0);
            LogRaw("ui: autoshow = %d (panel)", now ? 1 : 0);
            return 0;
        }
        if (id == IDC_DLS_RESETALL) {
            // Back to the DOCUMENTED defaults -- and `layers` is one of them
            // ("layers=1 ... a single pass, which is exactly how every version
            // before layers existed behaved", see app\dlssnr_dshow.ini.template).
            // Leaving the chain at three passes would not be a reset; the - half
            // of the stepper is not the only way back. This reverses the first
            // draft of this button, which deliberately kept the count.
            //
            // Order matters: shrink FIRST. SetLayerCount() clamps the selection,
            // so the DlssNrApply() below lands on layer 1, and the `layers=1` it
            // writes describes the chain we just collapsed to.
            //
            // The stored settings of layers 2..8 are deliberately NOT wiped: they
            // are inactive, and the documented behaviour is that a layer's keys
            // stay put until that layer is used again (the panel's + copies the
            // last layer in, so they are never resurrected by accident).
            Engine::Instance().SetLayerCount(1);
            const dlssnr::LayerCfg d = dlssnr::LayerDefault();
            DlssNrApply(true, d.style, d.pctIntensity, d.pctTone, d.pctStruct,
                        d.pctSkin, d.autoMask != 0, true);
            LogRaw("ui: reset to defaults (layer 1 of 1 back to documented defaults)");
            DlssNrSetChecked(s->chkEnable, true);
            DlssNrSetChecked(s->chkAutoMask, d.autoMask != 0);
            DlssNrSetChecked(s->chkDrop, true);       // dropping is the default
            DlssNrApplyDrop(true, true);
            DlssNrSetChecked(s->chkProc, false);      // reduced resolution is opt-in
            SendMessageW(s->trkProc, TBM_SETPOS, TRUE, 50);
            DlssNrApplyProcScale(false, 50, true);    // 50% = the value it restores to
            SendMessageW(s->trkExtrap, TBM_SETPOS, TRUE, 100);
            DlssNrApplyExtrap(100, true);             // 100 = off
            DlssNrRefreshControls(hwnd);
            if (s->onChanged) s->onChanged(s->onChangedCtx);
            HWND par = GetParent(hwnd);
            if (par) PostMessageW(par, DLS_N_CHANGED, 0, 0);
            return 0;
        }
        if (id == IDC_DLS_RECHECK) {
            DlssNrRefreshControls(hwnd);
            DlssNrRefreshStatus(hwnd);
            return 0;
        }
        if (id == IDC_DLS_CLOSE) { DestroyWindow(hwnd); return 0; }
        if (id == IDC_DLS_LOG) {
            wchar_t p[MAX_PATH];
            _snwprintf_s(p, _countof(p), _TRUNCATE, L"%s\\%s", g_dir, kLogName);
            ShellExecuteW(nullptr, L"open", L"notepad.exe", p, nullptr, SW_SHOWNORMAL);
            return 0;
        }
        break;
    }

    case WM_SYSCOMMAND:
        // No caption means no system menu, so make Alt+F4 explicit.
        if ((wp & 0xFFF0) == SC_CLOSE && s && s->standalone) { DestroyWindow(hwnd); return 0; }
        break;

    case WM_CLOSE:
        if (s && s->standalone) { DestroyWindow(hwnd); return 0; }
        break;

    case WM_DESTROY:
        if (s) {
            if (s->font)      DeleteObject(s->font);
            if (s->fontBold && s->fontBold != s->font) DeleteObject(s->fontBold);
            if (s->fontBig  && s->fontBig  != s->font) DeleteObject(s->fontBig);
            if (s->fontSmall&& s->fontSmall!= s->font) DeleteObject(s->fontSmall);
            s->font = s->fontBold = s->fontBig = s->fontSmall = nullptr;
        }
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// Windows 11 rounded corners, resolved at runtime so the build needs no
// dwmapi import library (and older Windows just ignores it).
static void DlssNrRoundCorners(HWND hwnd) {
    typedef HRESULT (WINAPI* SetAttrFn)(HWND, DWORD, LPCVOID, DWORD);
    static SetAttrFn setAttr = nullptr;
    static bool resolved = false;
    if (!resolved) {
        resolved = true;
        // The module handle is deliberately kept: the panel outlives this call.
        if (HMODULE dwm = LoadLibraryW(L"dwmapi.dll")) {
            setAttr = (SetAttrFn)(void*)GetProcAddress(dwm, "DwmSetWindowAttribute");
        }
    }
    if (!setAttr) return;
    const DWORD DWMWA_WINDOW_CORNER_PREFERENCE = 33;
    const DWORD DWMWCP_ROUND = 2;
    DWORD pref = DWMWCP_ROUND;
    setAttr(hwnd, DWMWA_WINDOW_CORNER_PREFERENCE, &pref, sizeof(pref));
}

static HWND DlssNrCreateSettings(HWND parent, bool standalone) {
    static bool classReady = false;
    if (!classReady) {
        WNDCLASSEXW wc = { sizeof(wc) };
        wc.lpfnWndProc   = DlssNrProc;
        wc.hInstance     = g_hModule;
        wc.hCursor       = LoadCursorW(nullptr, (LPCWSTR)IDC_ARROW);
        wc.hbrBackground = DlssNrBrush(kClrBg);
        wc.lpszClassName = L"DlssNrSettingsWnd";
        if (!RegisterClassExW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
            return nullptr;
        classReady = true;
    }
    DlssNrSettings* s = (DlssNrSettings*)calloc(1, sizeof(DlssNrSettings));
    if (!s) return nullptr;
    s->standalone = standalone;

    const int cx = DlssNrWantWidth(standalone), cy = DlssNrWantHeight(standalone);
    // WS_CLIPCHILDREN: WM_PAINT draws the strips, children draw themselves.
    // The standalone panel is a borderless popup with its own title bar (the
    // classic caption is what made it look like a Windows 7 dialog); WS_EX_
    // TOOLWINDOW keeps it out of the taskbar, which is where a tray-driven
    // utility belongs.
    const DWORD style = standalone ? (WS_POPUP | WS_CLIPCHILDREN)
                                   : (WS_CHILD | WS_CLIPCHILDREN);
    const DWORD exStyle = standalone ? WS_EX_TOOLWINDOW : 0;

    HWND h = CreateWindowExW(exStyle, L"DlssNrSettingsWnd",
                             standalone ? L"DLSSNR" : L"", style,
                             CW_USEDEFAULT, CW_USEDEFAULT, cx, cy,
                             parent, nullptr, g_hModule, s);
    if (!h) { free(s); return nullptr; }
    if (!standalone) {
        SetWindowPos(h, nullptr, 0, 0, cx, cy, SWP_NOZORDER | SWP_NOACTIVATE);
    } else {
        DlssNrRoundCorners(h);
        ShowWindow(h, SW_SHOW);
        UpdateWindow(h);
    }
    return h;
}

static HWND DlssNrShowSettingsWindow() {
    static HWND s_win = nullptr;
    if (s_win && IsWindow(s_win)) {
        ShowWindow(s_win, SW_SHOW);
        SetForegroundWindow(s_win);
        return s_win;
    }
    s_win = DlssNrCreateSettings(nullptr, true);
    return s_win;
}

static bool DlssNrSettingsVisible() {
    HWND h = FindWindowW(L"DlssNrSettingsWnd", nullptr);
    return h && IsWindowVisible(h) && !IsIconic(h);
}
// ---------------------------------------------------------------------------
// runtime-generated tray icon (keeps the repo free of .ico files)
// ---------------------------------------------------------------------------
static HICON DlssNrMakeIcon(int size) {
    HDC screen = GetDC(nullptr);
    HDC mem = CreateCompatibleDC(screen);

    BITMAPV5HEADER bh = {};
    bh.bV5Size = sizeof(bh);
    bh.bV5Width = size;
    bh.bV5Height = -size;                 // top-down
    bh.bV5Planes = 1;
    bh.bV5BitCount = 32;
    bh.bV5Compression = BI_BITFIELDS;
    bh.bV5RedMask   = 0x00FF0000;
    bh.bV5GreenMask = 0x0000FF00;
    bh.bV5BlueMask  = 0x000000FF;
    bh.bV5AlphaMask = 0xFF000000;

    void* bits = nullptr;
    HBITMAP color = CreateDIBSection(screen, (BITMAPINFO*)&bh, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!color) { DeleteDC(mem); ReleaseDC(nullptr, screen); return nullptr; }
    HGDIOBJ oldBmp = SelectObject(mem, color);

    RECT r = { 0, 0, size, size };
    HBRUSH bg = CreateSolidBrush(RGB(18, 52, 86));
    FillRect(mem, &r, bg);
    DeleteObject(bg);

    SetBkMode(mem, TRANSPARENT);
    SetTextColor(mem, RGB(255, 255, 255));
    HFONT f = CreateFontW(-(size * 13 / 16), 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
                          DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                          ANTIALIASED_QUALITY, DEFAULT_PITCH, L"Segoe UI");
    HGDIOBJ oldFont = SelectObject(mem, f);
    DrawTextW(mem, L"N", 1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    SelectObject(mem, oldFont);
    DeleteObject(f);
    SelectObject(mem, oldBmp);
    DeleteDC(mem);
    ReleaseDC(nullptr, screen);

    // GDI does not touch the alpha channel, so make the icon fully opaque.
    BYTE* px = (BYTE*)bits;
    for (int i = 0; i < size * size; ++i) px[i * 4 + 3] = 255;

    HBITMAP mask = CreateBitmap(size, size, 1, 1, nullptr);
    ICONINFO ii = {};
    ii.fIcon = TRUE;
    ii.hbmColor = color;
    ii.hbmMask = mask;
    HICON ic = CreateIconIndirect(&ii);
    DeleteObject(color);
    DeleteObject(mask);
    return ic;
}

// ---------------------------------------------------------------------------
// tray icon
// ---------------------------------------------------------------------------
enum {
    IDM_DLS_ENABLE   = 1,
    IDM_DLS_SETTINGS = 2,
    IDM_DLS_OPENLOG  = 3,
    IDM_DLS_ABOUT    = 4,
    IDM_DLS_HIDE     = 5,
    IDM_DLS_AUTOSHOW = 6,
    IDM_DLS_DROP     = 7,
    IDM_DLS_STYLE    = 100,     // +0..3
    IDM_DLS_INT      = 200,     // +0..4
    IDM_DLS_TONE     = 300,     // +0..4
    IDM_DLS_STRUCT   = 400,     // +0..4
    IDM_DLS_SKIN     = 500,     // +0..4 (kDlssNrPct presets)
    IDM_DLS_AUTOMASK = 600,
    IDM_DLS_LAYERCOUNT = 700,   // +0..kMaxLayers-1: 1..8 passes per frame
    IDM_DLS_SELLAYER   = 720,   // +0..kMaxLayers-1: which layer is edited
    IDM_DLS_MODEL      = 740,   // +preset: which network inside nvngx_dlssnr.dll
};

// Whether opening a video should pop the control panel automatically. Read from
// the ini every time so the tray menu can flip it live.
static bool DlssNrAutoshow() {
    wchar_t ini[MAX_PATH];
    DlssNrIniPath(ini, MAX_PATH);
    return GetPrivateProfileIntW(L"DLSSNR", L"autoshow", 1, ini) != 0;
}

#define DLS_WM_TRAY     (WM_APP + 20)
#define DLS_WM_QUIT     (WM_APP + 21)

class DlssNrTray {
public:
    static DlssNrTray& Instance() {
        static DlssNrTray* t = new DlssNrTray();
        return *t;
    }

    // Reference-counted by filter instances: the icon is present exactly while
    // at least one DLSSNR filter is in a graph, like LAV's.
    void FilterAdded()   {
        if (InterlockedIncrement(&m_filters) == 1) {
            m_autoShown = false;      // new playback session: allow one auto-open
            Start();
        }
    }
    void FilterRemoved() { if (InterlockedDecrement(&m_filters) <= 0) { InterlockedExchange(&m_filters, 0); Stop(); } }

private:
    volatile LONG    m_filters = 0;
    HANDLE           m_thread  = nullptr;
    DWORD            m_tid     = 0;
    HWND             m_hwnd    = nullptr;
    HICON            m_icon    = nullptr;
    bool             m_hidden  = false;
    volatile bool    m_autoShown = false;   // one auto-open per playback session
    // Signalled to ask the tray thread to tear its window down. It is an EVENT,
    // not a posted message, because the window may not exist yet when we ask:
    // the old code posted WM_QUIT to a null HWND, the thread never woke up, the
    // 5 s wait timed out and the thread (plus its icon) was leaked -- which is
    // what put TWO icons in the notification area.
    HANDLE           m_evtQuit = nullptr;
    CRITICAL_SECTION m_lock;

    DlssNrTray() {
        InitializeCriticalSection(&m_lock);
        m_evtQuit = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    }

    void Start() {
        EnterCriticalSection(&m_lock);
        if (!m_thread && m_evtQuit) {
            ResetEvent(m_evtQuit);
            m_hidden = false;
            m_thread = CreateThread(nullptr, 0, &DlssNrTray::ThreadProc, this, 0, &m_tid);
            if (!m_thread) LogRaw("tray: CreateThread failed");
        }
        LeaveCriticalSection(&m_lock);
    }

    void Stop() {
        EnterCriticalSection(&m_lock);
        if (m_thread) {
            SetEvent(m_evtQuit);                       // works even if the window is not up yet
            if (m_hwnd) PostMessageW(m_hwnd, DLS_WM_QUIT, 0, 0);
            WaitForSingleObject(m_thread, 5000);
            CloseHandle(m_thread);
            m_thread = nullptr;
            m_hwnd   = nullptr;
        }
        LeaveCriticalSection(&m_lock);
    }

    static DWORD WINAPI ThreadProc(LPVOID self) { ((DlssNrTray*)self)->Run(); return 0; }

    void Run() {
        INITCOMMONCONTROLSEX icc = { sizeof(icc), ICC_BAR_CLASSES | ICC_STANDARD_CLASSES };
        InitCommonControlsEx(&icc);

        WNDCLASSEXW wc = { sizeof(wc) };
        wc.lpfnWndProc   = DlssNrTray::Proc;
        wc.hInstance     = g_hModule;
        wc.lpszClassName = L"DlssNrTrayWnd";
        RegisterClassExW(&wc);

        m_hwnd = CreateWindowExW(0, L"DlssNrTrayWnd", L"DLSSNR", WS_POPUP,
                                 0, 0, 0, 0, nullptr, nullptr, g_hModule, this);
        if (!m_hwnd) { LogRaw("tray: CreateWindow failed err=%lu", GetLastError()); return; }

        // A filter can come and go before this thread even starts; do not add an
        // icon we would then have to tear down.
        if (WaitForSingleObject(m_evtQuit, 0) == WAIT_OBJECT_0) {
            DestroyWindow(m_hwnd);
            m_hwnd = nullptr;
            return;
        }

        int s = GetSystemMetrics(SM_CXSMICON);
        if (s <= 0) s = 16;
        m_icon = DlssNrMakeIcon(s);

        NOTIFYICONDATAW nid = {};
        nid.cbSize = sizeof(nid);
        nid.hWnd   = m_hwnd;
        nid.uID    = 1;
        nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
        nid.uCallbackMessage = DLS_WM_TRAY;
        nid.hIcon  = m_icon;
        wcsncpy_s(nid.szTip, L"DLSSNR", _TRUNCATE);
        if (!Shell_NotifyIconW(NIM_ADD, &nid)) {
            LogRaw("tray: Shell_NotifyIcon(NIM_ADD) failed err=%lu", GetLastError());
        } else {
            LogRaw("tray: icon added");
        }
        UpdateTip();
        SetTimer(m_hwnd, 1, 1000, nullptr);

        // Pump messages, but wake up immediately when Stop() signals the quit
        // event -- GetMessage() alone would block forever on a hidden window.
        for (;;) {
            DWORD w = MsgWaitForMultipleObjects(1, &m_evtQuit, FALSE, INFINITE, QS_ALLINPUT);
            if (w == WAIT_OBJECT_0) break;
            if (w != WAIT_OBJECT_0 + 1) break;
            MSG msg;
            while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
                if (msg.message == WM_QUIT) { w = WAIT_OBJECT_0; break; }
                TranslateMessage(&msg);
                DispatchMessageW(&msg);
            }
            if (w == WAIT_OBJECT_0) break;
        }

        Shell_NotifyIconW(NIM_DELETE, &nid);
        if (m_icon) { DestroyIcon(m_icon); m_icon = nullptr; }
        LogRaw("tray: icon removed");
    }

    void UpdateTip() {
        if (!m_hwnd || m_hidden) return;
        wchar_t tip[128];
        Engine& e = Engine::Instance();
        const wchar_t* eng = !g_shared ? T(L"未连接", L"n/a")
                          : (g_shared->engineReady ? T(L"就绪", L"ready")
                          : (g_shared->engineGaveUp ? T(L"初始化失败", L"failed")
                                                    : T(L"加载中", L"loading")));
        _snwprintf_s(tip, _countof(tip), _TRUNCATE,
                     T(L"DLSSNR  %s  引擎 %s  %dx%d", L"DLSSNR  %s  engine %s  %dx%d"),
                     e.OptEnabled() ? T(L"已启用", L"on") : T(L"已关闭", L"off"), eng,
                     g_shared ? (int)g_shared->videoW : 0,
                     g_shared ? (int)g_shared->videoH : 0);
        NOTIFYICONDATAW nid = {};
        nid.cbSize = sizeof(nid);
        nid.hWnd = m_hwnd; nid.uID = 1; nid.uFlags = NIF_TIP;
        wcsncpy_s(nid.szTip, tip, _TRUNCATE);
        Shell_NotifyIconW(NIM_MODIFY, &nid);
    }

    void SetIconVisible(bool visible) {
        NOTIFYICONDATAW nid = {};
        nid.cbSize = sizeof(nid);
        nid.hWnd = m_hwnd; nid.uID = 1;
        if (visible) {
            nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
            nid.uCallbackMessage = DLS_WM_TRAY;
            nid.hIcon = m_icon;
            wcsncpy_s(nid.szTip, L"DLSSNR", _TRUNCATE);
            Shell_NotifyIconW(NIM_ADD, &nid);
        } else {
            Shell_NotifyIconW(NIM_DELETE, &nid);
        }
        m_hidden = !visible;
    }

    // The panel is now a native window owned by this DLL, so re-opening it is
    // just ShowWindow on the same instance -- no second process, no singleton
    // mutex to fight, and no Python dependency.
    void OpenSettings() { DlssNrShowSettingsWindow(); }

    // Called from the tray timer: open the panel once per playback session if
    // the user asked for that. Runs on the tray thread, which owns the window.
    void AutoShowTick() {
        if (m_autoShown) return;
        if (!DlssNrAutoshow()) return;
        if (!g_shared || !g_shared->engineReady) return;
        m_autoShown = true;
        if (!DlssNrSettingsVisible()) {
            OpenSettings();
            LogRaw("ui: autoshow -> opened the control panel");
        }
    }

    void AppendPctMenuEx(HMENU parent, const wchar_t* title, int baseId,
                         int curPct, const int* values, int count) {
        HMENU sub = CreatePopupMenu();
        for (int i = 0; i < count; ++i) {
            wchar_t t[16];
            _snwprintf_s(t, _countof(t), _TRUNCATE, L"%d%%", values[i]);
            UINT f = MF_STRING | (values[i] == curPct ? MF_CHECKED : 0);
            AppendMenuW(sub, f, (UINT_PTR)(baseId + i), t);
        }
        AppendMenuW(parent, MF_POPUP, (UINT_PTR)sub, title);
    }



    void ShowMenu() {
        Engine& e = Engine::Instance();
        HMENU m = CreatePopupMenu();

        AppendMenuW(m, MF_STRING | (e.OptEnabled() ? MF_CHECKED : 0),
                    IDM_DLS_ENABLE, T(L"启用 DLSSNR", L"Enable DLSSNR"));

        // 层数 / Layers: the chain length. Built fresh on every open so the check
        // mark follows the live engine (which the panel can change too).
        HMENU lc = CreatePopupMenu();
        for (int i = 1; i <= (int)dlssnr::kMaxLayers; ++i) {
            wchar_t t[32];
            if (g_uiLang == 1)
                _snwprintf_s(t, _countof(t), _TRUNCATE, i == 1 ? L"%d layer" : L"%d layers", i);
            else
                _snwprintf_s(t, _countof(t), _TRUNCATE, L"%d 层", i);
            AppendMenuW(lc, MF_STRING | (e.OptLayerCount() == i ? MF_CHECKED : 0),
                        (UINT_PTR)(IDM_DLS_LAYERCOUNT + i - 1), t);
        }
        AppendMenuW(m, MF_POPUP, (UINT_PTR)lc, T(L"层数", L"Layers"));

        // 编辑层 / Edit layer: only the ACTIVE layers. Layer 5 of a 3-layer chain
        // is not something the engine could be asked to edit.
        HMENU sel = CreatePopupMenu();
        const int selCount = e.OptLayerCount() < (int)dlssnr::kMaxLayers
                           ? e.OptLayerCount() : (int)dlssnr::kMaxLayers;
        for (int i = 0; i < selCount; ++i) {
            wchar_t t[32];
            _snwprintf_s(t, _countof(t), _TRUNCATE,
                         g_uiLang == 1 ? L"Layer %d" : L"第 %d 层", i + 1);
            AppendMenuW(sel, MF_STRING | (e.OptSelLayer() == i ? MF_CHECKED : 0),
                        (UINT_PTR)(IDM_DLS_SELLAYER + i), t);
        }
        AppendMenuW(m, MF_POPUP, (UINT_PTR)sel, T(L"编辑层", L"Edit layer"));

        static const wchar_t* zhStyle[4] = { L"默认", L"自然", L"电影", L"风格3" };
        static const wchar_t* enStyle[4] = { L"Default", L"Natural", L"Cinema", L"Style 3" };
        HMENU st = CreatePopupMenu();
        for (int i = 0; i < kDlssNrStyleCount; ++i)
            AppendMenuW(st, MF_STRING | (e.OptStyle() == i ? MF_CHECKED : 0),
                        (UINT_PTR)(IDM_DLS_STYLE + i),
                        (g_uiLang == 1 ? enStyle : zhStyle)[i]);
        AppendMenuW(m, MF_POPUP, (UINT_PTR)st, T(L"风格", L"Style"));
        // Which network inside nvngx_dlssnr.dll, the same choices the panel's
        // model row offers. A popup cannot "cycle" the way a panel segment can, so
        // the raw ids get one item each (and a greyed-out fixed entry means this
        // DLL does not carry that network, exactly like the inert segment).
        {
            HMENU md = CreatePopupMenu();
            dlssnr::DllWeight wts[dlssnr::kDllWeightMax];
            const int nw = dlssnr::DllWeights(wts, dlssnr::kDllWeightMax);
            wchar_t lbl[32];
            for (int seg = 0; seg < kDlssNrModelBtnCustom; ++seg) {
                const bool can = DlssNrModelAvailable(DlssNrModelSegmentPreset(seg, e.OptModel()),
                                                      wts, nw);
                AppendMenuW(md, MF_STRING | (e.OptModel() == seg ? MF_CHECKED : 0) |
                                (can ? 0 : MF_GRAYED),
                            (UINT_PTR)(IDM_DLS_MODEL + seg),
                            DlssNrModelSegLabel(seg, e.OptModel(), lbl, _countof(lbl)));
            }
            AppendMenuW(md, MF_SEPARATOR, 0, nullptr);
            for (int id = kDlssNrModelBtnCustom; id <= (int)dlssnr::kMaxModelPreset; ++id) {
                if (g_uiLang == 1) _snwprintf_s(lbl, _countof(lbl), _TRUNCATE, L"Preset %d", id);
                else               _snwprintf_s(lbl, _countof(lbl), _TRUNCATE, L"预设 %d", id);
                AppendMenuW(md, MF_STRING | (e.OptModel() == id ? MF_CHECKED : 0),
                            (UINT_PTR)(IDM_DLS_MODEL + id), lbl);
            }
            AppendMenuW(m, MF_POPUP, (UINT_PTR)md, T(L"模型", L"Model"));
        }
        AppendPctMenuEx(m, T(L"强度", L"Intensity"), IDM_DLS_INT,
                        DlssNrPctMax(e.OptIntensity(), kDlssNrSliderMax[0]),
                        kDlssNrPct100, kDlssNrPctCount);
        AppendPctMenuEx(m, T(L"局部色调", L"Local tone"), IDM_DLS_TONE,
                        DlssNrPctMax(e.OptTone(), kDlssNrSliderMax[1]),
                        kDlssNrPct200, kDlssNrPctCount);
        AppendPctMenuEx(m, T(L"局部结构", L"Local structure"), IDM_DLS_STRUCT,
                        DlssNrPctMax(e.OptStruct(), kDlssNrSliderMax[2]),
                        kDlssNrPct200, kDlssNrPctCount);
        AppendPctMenuEx(m, T(L"皮肤结构", L"Skin structure"), IDM_DLS_SKIN,
                        DlssNrPctMax(e.OptSkin(), kDlssNrSliderMax[3]),
                        kDlssNrPct200, kDlssNrPctCount);
        AppendMenuW(m, MF_STRING | (e.OptAutoMask() ? MF_CHECKED : 0), IDM_DLS_AUTOMASK,
                    T(L"自动遮罩", L"Automatic mask"));
        AppendMenuW(m, MF_STRING | (e.OptDropFrames() ? MF_CHECKED : 0), IDM_DLS_DROP,
                    T(kDlssNrDropZh, kDlssNrDropEn));

        AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(m, MF_STRING, IDM_DLS_SETTINGS, T(L"打开控制面板…", L"Open control panel…"));
        AppendMenuW(m, MF_STRING | (DlssNrAutoshow() ? MF_CHECKED : 0), IDM_DLS_AUTOSHOW,
                    T(L"播放时自动显示控制面板", L"Show control panel on playback"));
        AppendMenuW(m, MF_STRING, IDM_DLS_OPENLOG,  T(L"打开日志", L"Open log"));
        AppendMenuW(m, MF_STRING, IDM_DLS_ABOUT,    T(L"关于 DLSSNR", L"About DLSSNR"));
        AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(m, MF_STRING, IDM_DLS_HIDE,     T(L"隐藏图标", L"Hide icon"));

        POINT pt;
        GetCursorPos(&pt);
        SetForegroundWindow(m_hwnd);
        int cmd = (int)TrackPopupMenu(m, TPM_RETURNCMD | TPM_RIGHTBUTTON,
                                      pt.x, pt.y, 0, m_hwnd, nullptr);
        DestroyMenu(m);
        PostMessageW(m_hwnd, WM_NULL, 0, 0);
        if (cmd) ApplyCommand(cmd);
    }

    void ApplyCommand(int cmd) {
        Engine& e = Engine::Instance();
        if (cmd == IDM_DLS_ENABLE)  { DlssNrApply(!e.OptEnabled(), e.OptStyle(), Pct(e.OptIntensity()), Pct(e.OptTone()), Pct(e.OptStruct()), Pct(e.OptSkin()), e.OptAutoMask(), true); UpdateTip(); return; }
        if (cmd == IDM_DLS_AUTOMASK){ DlssNrApply(e.OptEnabled(), e.OptStyle(), Pct(e.OptIntensity()), Pct(e.OptTone()), Pct(e.OptStruct()), Pct(e.OptSkin()), !e.OptAutoMask(), true); return; }
        if (cmd == IDM_DLS_DROP)    { DlssNrApplyDrop(!e.OptDropFrames(), true); return; }
        if (cmd >= IDM_DLS_LAYERCOUNT && cmd < IDM_DLS_LAYERCOUNT + (int)dlssnr::kMaxLayers) {
            e.SetLayerCount(cmd - IDM_DLS_LAYERCOUNT + 1);
            // No "current layer" edit happened, so persist the whole chain: the
            // count plus every value, including the copy SetLayerCount() just made
            // into the new slots.
            DlssNrWriteLayers();
            DlssNrRefreshSinks(true);   // a paused graph must build the extra features
            UpdateTip();
            return;
        }
        if (cmd >= IDM_DLS_SELLAYER && cmd < IDM_DLS_SELLAYER + (int)dlssnr::kMaxLayers) {
            // Only the edit target changes; nothing to push and nothing to write.
            // An open panel picks the new selection up on its next timer tick.
            e.SetSelLayer(cmd - IDM_DLS_SELLAYER);
            return;
        }
        if (cmd >= IDM_DLS_MODEL && cmd <= IDM_DLS_MODEL + (int)dlssnr::kMaxModelPreset) {
            DlssNrApplyModel(cmd - IDM_DLS_MODEL, true);
            return;
        }
        if (cmd == IDM_DLS_SETTINGS){ OpenSettings(); return; }
        if (cmd == IDM_DLS_AUTOSHOW) {
            bool now = !DlssNrAutoshow();
            DlssNrIniWriteInt(L"autoshow", now ? 1 : 0);
            m_autoShown = now ? true : false;    // don't pop it right after enabling
            LogRaw("ui: autoshow = %d", now ? 1 : 0);
            return;
        }
        if (cmd == IDM_DLS_OPENLOG) { OpenLog(); return; }
        if (cmd == IDM_DLS_HIDE)    { SetIconVisible(false); return; }
        if (cmd == IDM_DLS_ABOUT) {
            MessageBoxW(m_hwnd,
                T(L"DLSS Neural Render (DLSSNR)\n\n"
                  L"DirectShow 滤镜 + 原生控制面板，无 Python 依赖。\n"
                  L"引擎 nvngx_dlssnr.dll 需自行准备，不随本软件分发。\n\n"
                  L"MIT License — Cyanke",
                  L"DLSS Neural Render (DLSSNR)\n\n"
                  L"DirectShow filter with a native control panel; no Python needed.\n"
                  L"The engine nvngx_dlssnr.dll must be obtained separately.\n\n"
                  L"MIT License — Cyanke"),
                T(L"关于 DLSSNR", L"About DLSSNR"), MB_OK | MB_ICONINFORMATION);
            return;
        }
        if (cmd >= IDM_DLS_STYLE && cmd < IDM_DLS_STYLE + 4) {
            DlssNrApply(e.OptEnabled(), cmd - IDM_DLS_STYLE, Pct(e.OptIntensity()), Pct(e.OptTone()), Pct(e.OptStruct()), Pct(e.OptSkin()), e.OptAutoMask(), true);
            return;
        }
        if (cmd >= IDM_DLS_SKIN && cmd < IDM_DLS_SKIN + kDlssNrPctCount) {
            DlssNrApply(e.OptEnabled(), e.OptStyle(), Pct(e.OptIntensity()), Pct(e.OptTone()), Pct(e.OptStruct()), kDlssNrPct200[cmd - IDM_DLS_SKIN], e.OptAutoMask(), true);
            return;
        }
        struct { int base; const wchar_t* key; int idx; const int* vals; } grp[3] = {
            { IDM_DLS_INT,    L"intensity",   0, kDlssNrPct100 },
            { IDM_DLS_TONE,   L"localtone",   1, kDlssNrPct200 },
            { IDM_DLS_STRUCT, L"localstruct", 2, kDlssNrPct200 },
        };
        for (auto& g : grp) {
            if (cmd >= g.base && cmd < g.base + kDlssNrPctCount) {
                int pct[3] = { Pct(e.OptIntensity()), Pct(e.OptTone()), Pct(e.OptStruct()) };
                pct[g.idx] = g.vals[cmd - g.base];
                DlssNrApply(e.OptEnabled(), e.OptStyle(), pct[0], pct[1], pct[2], Pct(e.OptSkin()), e.OptAutoMask(), true);
                return;
            }
        }
    }

    static int Pct(float v) { return DlssNrPctMax(v, 200); }

    void OpenLog() {
        wchar_t p[MAX_PATH];
        _snwprintf_s(p, _countof(p), _TRUNCATE, L"%s\\%s", g_dir, kLogName);
        ShellExecuteW(nullptr, L"open", L"notepad.exe", p, nullptr, SW_SHOWNORMAL);
    }

    static LRESULT CALLBACK Proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
        DlssNrTray* self = (DlssNrTray*)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
        switch (msg) {
        case WM_CREATE:
            SetWindowLongPtrW(hwnd, GWLP_USERDATA,
                              (LONG_PTR)((CREATESTRUCTW*)lp)->lpCreateParams);
            return 0;
        case WM_TIMER:
            if (self) { self->UpdateTip(); self->AutoShowTick(); }
            return 0;
        case DLS_WM_TRAY:
            if (self) {
                UINT ev = (UINT)LOWORD(lp);
                if (ev == WM_RBUTTONUP || ev == WM_CONTEXTMENU) self->ShowMenu();
                else if (ev == WM_LBUTTONDBLCLK) self->OpenSettings();
            }
            return 0;
        case DLS_WM_QUIT:
            DestroyWindow(hwnd);
            return 0;
        case WM_DESTROY:
            KillTimer(hwnd, 1);
            PostQuitMessage(0);
            return 0;
        }
        return DefWindowProcW(hwnd, msg, wp, lp);
    }
};