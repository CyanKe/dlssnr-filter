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
static const int kDlssNrPct[5] = { 0, 25, 50, 75, 100 };
static const int kDlssNrPctCount = 5;

// The single place that mutates the live engine and, optionally, the ini.
static void DlssNrApply(bool enabled, int style, int pctInt, int pctTone, int pctStruct,
                        bool persist) {
    Engine::Instance().SetOptions(enabled, style,
                                  pctInt / 100.0f, pctTone / 100.0f, pctStruct / 100.0f);
    if (!persist) return;
    DlssNrIniWriteInt(L"enabled",     enabled ? 1 : 0);
    DlssNrIniWriteInt(L"style",       style);
    DlssNrIniWriteInt(L"intensity",   pctInt);
    DlssNrIniWriteInt(L"localtone",   pctTone);
    DlssNrIniWriteInt(L"localstruct", pctStruct);
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
// (1 = planar YUV 4:2:0, 3 = BGR24, 4 = BGR32).
static const wchar_t* DlssNrFmtName(int bpp) {
    if (bpp == 3) return T(L"BGR24（最优）", L"BGR24 (best)");
    if (bpp == 4) return T(L"BGR32", L"BGR32");
    if (bpp == 1) return T(L"NV12（内部转 BGR24）", L"NV12 (converted to BGR24)");
    return T(L"未知", L"unknown");
}

#define DLS_N_CHANGED   (WM_APP + 10)   // posted to the parent on any edit

enum {
    IDC_DLS_ENABLE  = 1001,
    IDC_DLS_STYLE   = 1002,
    IDC_DLS_RESET   = 1004,
    IDC_DLS_RECHECK = 1005,
    IDC_DLS_LOG     = 1006,
    IDC_DLS_TITLE   = 1007,
    IDC_DLS_SUB     = 1008,
    IDC_DLS_STATS   = 1009,
    IDC_DLS_GEOM    = 1010,
    IDC_DLS_HINT    = 1011,
    IDC_DLS_GRP1    = 1012,
    IDC_DLS_GRP2    = 1013,
    IDC_DLS_LANG    = 1014,
    IDC_DLS_LANGLBL = 1015,
    IDC_DLS_AUTOSHOW= 1016,
    IDC_DLS_LBL0    = 1020, IDC_DLS_TRK0 = 1021, IDC_DLS_VAL0 = 1022,
    IDC_DLS_LBL1    = 1030, IDC_DLS_TRK1 = 1031, IDC_DLS_VAL1 = 1032,
    IDC_DLS_LBL2    = 1040, IDC_DLS_TRK2 = 1041, IDC_DLS_VAL2 = 1042,
    IDC_DLS_STYLELBL= 1050,
};

struct DlssNrSettings {
    HWND hwnd;
    HWND grp1, grp2;
    HWND lblTitle, lblSub, lblStats, lblGeom, lblHint;
    HWND chkEnable, lblStyle, cboStyle;
    HWND lblLang, cboLang, chkAutoshow;
    HWND lbl[3], trk[3], val[3];
    HWND btnReset, btnRecheck, btnLog;
    HFONT font, fontBig, fontSmall;
    bool  standalone;
    void (*onChanged)(void*);
    void* onChangedCtx;
};

// ---- geometry (client coordinates, all in px) ------------------------------
static const int kM      = 12;    // outer margin
static const int kGW     = 512;   // group width
static const int kIndent = 14;    // control indent inside a group
static const int kRowH   = 20;
static const int kLblW   = 78;
static const int kLangLblW = 124;  // fits "English/Language"
static const int kBarW   = 250;
static const int kValW   = 66;

static const int kG1Y = 10,  kG1H = 112;
static const int kG2Y = 128, kG2H = 210;
static const int kHintY = kG2Y + kG2H + 8;
static const int kHintH = 46;   // the hint wraps to two lines
static const int kBtnY  = kHintY + kHintH + 8;
static const int kBtnH  = 28;

static int DlssNrWantWidth()  { return kM * 2 + kGW; }              // 480
static int DlssNrWantHeight() { return kBtnY + kBtnH + kM; }        // ~408

static void DlssNrLayout(DlssNrSettings* s) {
    RECT rc; GetClientRect(s->hwnd, &rc);
    int x = kM;
    int ix = kM + kIndent;
    int iw = kGW - 2 * kIndent;

    MoveWindow(s->grp1, x, kG1Y, kGW, kG1H, TRUE);
    MoveWindow(s->grp2, x, kG2Y, kGW, kG2H, TRUE);

    int y = kG1Y + 22;
    MoveWindow(s->lblTitle, ix, y, iw, 26, TRUE);  y += 28;
    MoveWindow(s->lblSub,   ix, y, iw, 17, TRUE);  y += 18;
    MoveWindow(s->lblStats, ix, y, iw, 17, TRUE);  y += 18;
    MoveWindow(s->lblGeom,  ix, y, iw, 17, TRUE);

    y = kG2Y + 22;
    MoveWindow(s->chkEnable,   ix,               y, iw / 2 - 4, 22, TRUE);
    MoveWindow(s->chkAutoshow, ix + iw / 2,      y, iw / 2,     22, TRUE);
    y += 26;
    MoveWindow(s->lblStyle, ix, y + 3, kLblW, kRowH, TRUE);
    MoveWindow(s->cboStyle, ix + kLblW, y - 1, iw - kLblW, 220, TRUE);
    y += 28;
    for (int i = 0; i < 3; ++i) {
        MoveWindow(s->lbl[i], ix, y + 3, kLblW, kRowH, TRUE);
        MoveWindow(s->trk[i], ix + kLblW, y, kBarW, kRowH, TRUE);
        MoveWindow(s->val[i], ix + kLblW + kBarW + 6, y + 3, kValW, kRowH, TRUE);
        y += 28;
    }
    MoveWindow(s->lblLang, ix, y + 3, kLangLblW, kRowH, TRUE);
    MoveWindow(s->cboLang, ix + kLangLblW, y - 1, 150, 220, TRUE);

    MoveWindow(s->lblHint, kM, kHintY, kGW, kHintH, TRUE);

    const int gap = 9;
    int bw = (kGW - 2 * gap) / 3;
    MoveWindow(s->btnReset,   kM,                     kBtnY, bw, kBtnH, TRUE);
    MoveWindow(s->btnRecheck, kM + bw + gap,          kBtnY, bw, kBtnH, TRUE);
    MoveWindow(s->btnLog,     kM + 2 * (bw + gap),    kBtnY, bw, kBtnH, TRUE);
}

// ---- text (re-applied whenever the language changes) -----------------------
static void DlssNrRetext(HWND hwnd) {
    DlssNrSettings* s = (DlssNrSettings*)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
    if (!s) return;
    SetWindowTextW(hwnd, s->standalone ? T(L"DLSSNR 控制面板", L"DLSSNR Control Panel")
                                       : T(L"DLSSNR", L"DLSSNR"));
    SetWindowTextW(s->grp1, T(L" 滤镜状态 ", L" Filter status "));
    SetWindowTextW(s->grp2, T(L" 参数（实时生效） ", L" Parameters (live) "));
    SetWindowTextW(s->chkEnable, T(L"启用 DLSSNR", L"Enable DLSSNR"));
    SetWindowTextW(s->chkAutoshow, T(L"启动时显示控制面板", L"Show control panel at startup"));
    SetWindowTextW(s->lblStyle, T(L"风格", L"Style"));
    SetWindowTextW(s->lblLang, (g_uiLang == 1) ? L"English/Language" : L"中文/Language");
    SetWindowTextW(s->lbl[0],   T(L"强度",     L"Intensity"));
    SetWindowTextW(s->lbl[1],   T(L"局部色调", L"Local tone"));
    SetWindowTextW(s->lbl[2],   T(L"局部结构", L"Local structure"));
    SetWindowTextW(s->btnReset,   T(L"重置为默认", L"Reset defaults"));
    SetWindowTextW(s->btnRecheck, T(L"重新检测",   L"Re-check"));
    SetWindowTextW(s->btnLog,     T(L"打开日志",   L"Open log"));
    SetWindowTextW(s->lblHint, T(
        L"参数实时生效；取消勾选 = 真正的直通（逐字节等于输入），强度拉到 0 仍会跑引擎。",
        L"Changes apply live. Unchecking = true passthrough (byte-identical). "
        L"Intensity 0 still runs the engine."));

    // style + language combo contents differ per language
    int curStyle = (int)SendMessageW(s->cboStyle, CB_GETCURSEL, 0, 0);
    SendMessageW(s->cboStyle, CB_RESETCONTENT, 0, 0);
    static const wchar_t* zh[4] = { L"默认", L"自然", L"电影", L"风格3" };
    static const wchar_t* en[4] = { L"Default", L"Natural", L"Cinema", L"Style 3" };
    for (int i = 0; i < 4; ++i)
        SendMessageW(s->cboStyle, CB_ADDSTRING, 0, (LPARAM)(g_uiLang == 1 ? en[i] : zh[i]));
    SendMessageW(s->cboStyle, CB_SETCURSEL, curStyle < 0 ? 0 : curStyle, 0);

    int curLang = (int)SendMessageW(s->cboLang, CB_GETCURSEL, 0, 0);
    SendMessageW(s->cboLang, CB_RESETCONTENT, 0, 0);
    SendMessageW(s->cboLang, CB_ADDSTRING, 0, (LPARAM)L"中文");
    SendMessageW(s->cboLang, CB_ADDSTRING, 0, (LPARAM)L"English");
    SendMessageW(s->cboLang, CB_SETCURSEL, curLang < 0 ? g_uiLang : curLang, 0);
}

static void DlssNrRefreshControls(HWND hwnd) {
    DlssNrSettings* s = (DlssNrSettings*)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
    if (!s) return;
    Engine& e = Engine::Instance();
    SendMessageW(s->chkEnable, BM_SETCHECK, e.OptEnabled() ? BST_CHECKED : BST_UNCHECKED, 0);
    SendMessageW(s->chkAutoshow, BM_SETCHECK, DlssNrAutoshow() ? BST_CHECKED : BST_UNCHECKED, 0);
    SendMessageW(s->cboStyle,  CB_SETCURSEL, (WPARAM)(e.OptStyle() & 3), 0);
    float v[3] = { e.OptIntensity(), e.OptTone(), e.OptStruct() };
    for (int i = 0; i < 3; ++i) {
        int pct = (int)(v[i] * 100.0f + 0.5f);
        if (pct < 0) pct = 0;
        if (pct > 100) pct = 100;
        SendMessageW(s->trk[i], TBM_SETPOS, TRUE, pct);
        wchar_t t[16];
        _snwprintf_s(t, _countof(t), _TRUNCATE, L"%d%%", pct);
        SetWindowTextW(s->val[i], t);
    }
}

static void DlssNrPush(HWND hwnd, bool persist) {
    DlssNrSettings* s = (DlssNrSettings*)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
    if (!s) return;
    bool en = SendMessageW(s->chkEnable, BM_GETCHECK, 0, 0) == BST_CHECKED;
    int style = (int)SendMessageW(s->cboStyle, CB_GETCURSEL, 0, 0);
    if (style < 0) style = 0;
    int pct[3];
    for (int i = 0; i < 3; ++i)
        pct[i] = (int)SendMessageW(s->trk[i], TBM_GETPOS, 0, 0);
    DlssNrApply(en, style, pct[0], pct[1], pct[2], persist);
    if (s->onChanged) s->onChanged(s->onChangedCtx);
    HWND par = GetParent(hwnd);
    if (par) PostMessageW(par, DLS_N_CHANGED, 0, 0);
}

static COLORREF g_statusColour = RGB(0x33, 0x33, 0x33);

static void DlssNrRefreshStatus(HWND hwnd) {
    DlssNrSettings* s = (DlssNrSettings*)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
    if (!s) return;
    if (!g_shared) {
        SetWindowTextW(s->lblTitle, T(L"未连接", L"Not connected"));
        SetWindowTextW(s->lblSub,   T(L"共享内存不可用", L"Shared memory unavailable"));
        SetWindowTextW(s->lblStats, L"");
        SetWindowTextW(s->lblGeom,  L"");
        return;
    }
    SharedState* sh = g_shared;
    const wchar_t* eng = sh->engineReady ? T(L"就绪", L"ready")
                      : (sh->engineGaveUp ? T(L"失败", L"failed") : T(L"加载中", L"loading"));
    const wchar_t* colourTxt;
    COLORREF col = RGB(0x33, 0x33, 0x33);
    switch ((int)sh->lastReason) {
    case 0: colourTxt = T(L"正在处理", L"Processing");                 col = RGB(0x0A,0x7D,0x0A); break;
    case 1: colourTxt = T(L"引擎加载中…（约 1-15 秒）", L"Loading model… (1-15 s)"); col = RGB(0xB0,0x6A,0x00); break;
    case 2: colourTxt = T(L"已关闭（面板或 ini）", L"Disabled (panel or ini)");     col = RGB(0x60,0x60,0x60); break;
    case 4: colourTxt = T(L"分辨率与引擎会话不一致", L"Resolution differs from engine session"); col = RGB(0xC0,0,0); break;
    case 5: colourTxt = T(L"引擎返回错误，已直通", L"Engine error, passing through"); col = RGB(0xC0,0,0); break;
    case 6: colourTxt = T(L"引擎初始化失败（详见日志）", L"Engine init failed (see log)"); col = RGB(0xC0,0,0); break;
    default: colourTxt = T(L"未知状态", L"Unknown");                    col = RGB(0x60,0x60,0x60); break;
    }
    if ((int)sh->heartbeat == 0) colourTxt = T(L"等待滤镜开始处理…", L"Waiting for the filter…");
    g_statusColour = col;
    SetWindowTextW(s->lblTitle, colourTxt);
    InvalidateRect(s->lblTitle, nullptr, TRUE);

    wchar_t buf[320];
    _snwprintf_s(buf, _countof(buf), _TRUNCATE, L"PID %d    %s", (int)sh->pid, sh->status);
    SetWindowTextW(s->lblSub, buf);

    int seen = (int)sh->framesSeen, proc = (int)sh->framesProcessed;
    _snwprintf_s(buf, _countof(buf), _TRUNCATE,
                 T(L"帧: 收到 %d / 处理 %d / 直通 %d  (处理率 %.1f%%)    引擎 %s    %.2f ms/帧",
                   L"Frames: seen %d / processed %d / passthrough %d  (%.1f%%)    engine %s    %.2f ms/frame"),
                 seen, proc, (int)sh->framesPassthrough,
                 seen ? (100.0 * proc / seen) : 0.0, eng, (double)sh->lastProcessMs);
    SetWindowTextW(s->lblStats, buf);

    _snwprintf_s(buf, _countof(buf), _TRUNCATE,
                 T(L"视频 %dx%d    像素格式 %s    引擎会话 %dx%d",
                   L"Video %dx%d    format %s    engine session %dx%d"),
                 (int)sh->videoW, (int)sh->videoH, DlssNrFmtName((int)sh->inputBpp),
                 (int)sh->engineW, (int)sh->engineH);
    SetWindowTextW(s->lblGeom, buf);
}

static LRESULT CALLBACK DlssNrProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    DlssNrSettings* s = (DlssNrSettings*)GetWindowLongPtrW(hwnd, GWLP_USERDATA);

    switch (msg) {
    case WM_CREATE: {
        s = (DlssNrSettings*)((CREATESTRUCTW*)lp)->lpCreateParams;
        s->hwnd = hwnd;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)s);

        NONCLIENTMETRICSW ncm = { sizeof(NONCLIENTMETRICSW) };
        if (SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0)) {
            s->font = CreateFontIndirectW(&ncm.lfMessageFont);
            LOGFONTW big = ncm.lfMessageFont;
            big.lfHeight = (LONG)(ncm.lfMessageFont.lfHeight * 1.35);
            big.lfWeight = FW_BOLD;
            s->fontBig = CreateFontIndirectW(&big);
            LOGFONTW sm = ncm.lfMessageFont;
            sm.lfHeight = (LONG)(ncm.lfMessageFont.lfHeight * 0.95);
            s->fontSmall = CreateFontIndirectW(&sm);
        } else {
            s->font = s->fontBig = s->fontSmall = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
        }
        DlssNrLoadLang();

        // Group boxes must be created first so they stay behind the controls.
        s->grp1 = CreateWindowExW(0, L"BUTTON", L"", WS_CHILD|WS_VISIBLE|BS_GROUPBOX,
                                  0,0,0,0, hwnd, (HMENU)(INT_PTR)IDC_DLS_GRP1, g_hModule, nullptr);
        s->grp2 = CreateWindowExW(0, L"BUTTON", L"", WS_CHILD|WS_VISIBLE|BS_GROUPBOX,
                                  0,0,0,0, hwnd, (HMENU)(INT_PTR)IDC_DLS_GRP2, g_hModule, nullptr);

        s->lblTitle = CreateWindowExW(0, L"STATIC", L"", WS_CHILD|WS_VISIBLE|SS_LEFT,
                                      0,0,0,0, hwnd, (HMENU)(INT_PTR)IDC_DLS_TITLE, g_hModule, nullptr);
        s->lblSub   = CreateWindowExW(0, L"STATIC", L"", WS_CHILD|WS_VISIBLE|SS_LEFT|SS_ENDELLIPSIS,
                                      0,0,0,0, hwnd, (HMENU)(INT_PTR)IDC_DLS_SUB, g_hModule, nullptr);
        s->lblStats = CreateWindowExW(0, L"STATIC", L"", WS_CHILD|WS_VISIBLE|SS_LEFT|SS_ENDELLIPSIS,
                                      0,0,0,0, hwnd, (HMENU)(INT_PTR)IDC_DLS_STATS, g_hModule, nullptr);
        s->lblGeom  = CreateWindowExW(0, L"STATIC", L"", WS_CHILD|WS_VISIBLE|SS_LEFT|SS_ENDELLIPSIS,
                                      0,0,0,0, hwnd, (HMENU)(INT_PTR)IDC_DLS_GEOM, g_hModule, nullptr);
        s->lblHint  = CreateWindowExW(0, L"STATIC", L"", WS_CHILD|WS_VISIBLE|SS_LEFT,
                                      0,0,0,0, hwnd, (HMENU)(INT_PTR)IDC_DLS_HINT, g_hModule, nullptr);
        s->chkEnable= CreateWindowExW(0, L"BUTTON", L"", WS_CHILD|WS_VISIBLE|BS_AUTOCHECKBOX,
                                      0,0,0,0, hwnd, (HMENU)(INT_PTR)IDC_DLS_ENABLE, g_hModule, nullptr);
        s->chkAutoshow = CreateWindowExW(0, L"BUTTON", L"", WS_CHILD|WS_VISIBLE|BS_AUTOCHECKBOX,
                                      0,0,0,0, hwnd, (HMENU)(INT_PTR)IDC_DLS_AUTOSHOW, g_hModule, nullptr);
        s->lblStyle = CreateWindowExW(0, L"STATIC", L"", WS_CHILD|WS_VISIBLE|SS_LEFT,
                                      0,0,0,0, hwnd, (HMENU)(INT_PTR)IDC_DLS_STYLELBL, g_hModule, nullptr);
        s->cboStyle = CreateWindowExW(0, L"COMBOBOX", L"",
                                      WS_CHILD|WS_VISIBLE|CBS_DROPDOWNLIST|WS_VSCROLL,
                                      0,0,0,0, hwnd, (HMENU)(INT_PTR)IDC_DLS_STYLE, g_hModule, nullptr);
        s->lblLang  = CreateWindowExW(0, L"STATIC", L"", WS_CHILD|WS_VISIBLE|SS_LEFT,
                                      0,0,0,0, hwnd, (HMENU)(INT_PTR)IDC_DLS_LANGLBL, g_hModule, nullptr);
        s->cboLang  = CreateWindowExW(0, L"COMBOBOX", L"",
                                      WS_CHILD|WS_VISIBLE|CBS_DROPDOWNLIST|WS_VSCROLL,
                                      0,0,0,0, hwnd, (HMENU)(INT_PTR)IDC_DLS_LANG, g_hModule, nullptr);

        for (int i = 0; i < 3; ++i) {
            int base = IDC_DLS_TRK0 + i * 10;
            s->lbl[i] = CreateWindowExW(0, L"STATIC", L"", WS_CHILD|WS_VISIBLE|SS_LEFT,
                                        0,0,0,0, hwnd, (HMENU)(INT_PTR)(base - 1), g_hModule, nullptr);
            s->trk[i] = CreateWindowExW(0, TRACKBAR_CLASSW, L"",
                                        WS_CHILD|WS_VISIBLE|TBS_HORZ|TBS_AUTOTICKS,
                                        0,0,0,0, hwnd, (HMENU)(INT_PTR)base, g_hModule, nullptr);
            SendMessageW(s->trk[i], TBM_SETRANGE, TRUE, MAKELPARAM(0, 100));
            SendMessageW(s->trk[i], TBM_SETTICFREQ, 25, 0);
            s->val[i] = CreateWindowExW(0, L"STATIC", L"100%", WS_CHILD|WS_VISIBLE|SS_RIGHT,
                                        0,0,0,0, hwnd, (HMENU)(INT_PTR)(base + 2), g_hModule, nullptr);
        }

        s->btnReset   = CreateWindowExW(0, L"BUTTON", L"", WS_CHILD|WS_VISIBLE|BS_PUSHBUTTON,
                                        0,0,0,0, hwnd, (HMENU)(INT_PTR)IDC_DLS_RESET, g_hModule, nullptr);
        s->btnRecheck = CreateWindowExW(0, L"BUTTON", L"", WS_CHILD|WS_VISIBLE|BS_PUSHBUTTON,
                                        0,0,0,0, hwnd, (HMENU)(INT_PTR)IDC_DLS_RECHECK, g_hModule, nullptr);
        s->btnLog     = CreateWindowExW(0, L"BUTTON", L"", WS_CHILD|WS_VISIBLE|BS_PUSHBUTTON,
                                        0,0,0,0, hwnd, (HMENU)(INT_PTR)IDC_DLS_LOG, g_hModule, nullptr);

        HWND all[] = { s->grp1, s->grp2, s->lblTitle, s->lblSub, s->lblStats, s->lblGeom,
                       s->lblHint, s->chkEnable, s->chkAutoshow, s->lblStyle, s->cboStyle, s->lblLang, s->cboLang,
                       s->lbl[0], s->trk[0], s->val[0], s->lbl[1], s->trk[1], s->val[1],
                       s->lbl[2], s->trk[2], s->val[2],
                       s->btnReset, s->btnRecheck, s->btnLog };
        for (HWND h : all) if (h) SendMessageW(h, WM_SETFONT, (WPARAM)s->font, TRUE);
        SendMessageW(s->lblTitle, WM_SETFONT, (WPARAM)s->fontBig, TRUE);
        SendMessageW(s->lblSub,   WM_SETFONT, (WPARAM)s->fontSmall, TRUE);
        SendMessageW(s->lblStats, WM_SETFONT, (WPARAM)s->fontSmall, TRUE);
        SendMessageW(s->lblGeom,  WM_SETFONT, (WPARAM)s->fontSmall, TRUE);
        SendMessageW(s->lblHint,  WM_SETFONT, (WPARAM)s->fontSmall, TRUE);

        DlssNrRetext(hwnd);
        DlssNrRefreshControls(hwnd);
        DlssNrRefreshStatus(hwnd);
        SetTimer(hwnd, 1, 500, nullptr);
        return 0;
    }

    case WM_CTLCOLORSTATIC: {
        HWND c = (HWND)lp;
        if (c == s->lblTitle) {
            SetTextColor((HDC)wp, g_statusColour);
            SetBkMode((HDC)wp, TRANSPARENT);
            return (LRESULT)GetSysColorBrush(COLOR_BTNFACE);
        }
        if (c == s->lblSub || c == s->lblStats || c == s->lblGeom) {
            SetBkMode((HDC)wp, TRANSPARENT);
            return (LRESULT)GetSysColorBrush(COLOR_BTNFACE);
        }
        break;
    }
    case WM_SIZE:
        if (s) DlssNrLayout(s);
        return 0;
    case WM_TIMER:
        DlssNrRefreshStatus(hwnd);
        if (s) SendMessageW(s->chkAutoshow, BM_SETCHECK,
                            DlssNrAutoshow() ? BST_CHECKED : BST_UNCHECKED, 0);
        return 0;
    case WM_HSCROLL: {
        if (!s) break;
        HWND from = (HWND)lp;
        int idx = -1;
        for (int i = 0; i < 3; ++i) if (from == s->trk[i]) idx = i;
        if (idx < 0) break;
        int pct = (int)SendMessageW(from, TBM_GETPOS, 0, 0);
        wchar_t t[16];
        _snwprintf_s(t, _countof(t), _TRUNCATE, L"%d%%", pct);
        SetWindowTextW(s->val[idx], t);
        DlssNrPush(hwnd, LOWORD(wp) == TB_ENDTRACK);
        return 0;
    }
    case WM_COMMAND: {
        if (!s) break;
        int id = LOWORD(wp), code = HIWORD(wp);
        if (id == IDC_DLS_LANG && code == CBN_SELCHANGE) {
            int sel = (int)SendMessageW(s->cboLang, CB_GETCURSEL, 0, 0);
            g_uiLang = (sel == 1) ? 1 : 0;
            wchar_t ini[MAX_PATH]; DlssNrIniPath(ini, MAX_PATH);
            WritePrivateProfileStringW(L"DLSSNR", L"lang", g_uiLang == 1 ? L"en" : L"zh", ini);
            DlssNrRetext(hwnd);
            DlssNrRefreshStatus(hwnd);
            return 0;
        }
        if (id == IDC_DLS_RESET) {
            DlssNrApply(true, 0, 100, 100, 100, true);
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
        if (id == IDC_DLS_LOG) {
            wchar_t p[MAX_PATH];
            _snwprintf_s(p, _countof(p), _TRUNCATE, L"%s\\%s", g_dir, kLogName);
            ShellExecuteW(nullptr, L"open", L"notepad.exe", p, nullptr, SW_SHOWNORMAL);
            return 0;
        }
        if (id == IDC_DLS_ENABLE && code == BN_CLICKED)    { DlssNrPush(hwnd, true); return 0; }
        if (id == IDC_DLS_AUTOSHOW && code == BN_CLICKED) {
            bool now = SendMessageW(s->chkAutoshow, BM_GETCHECK, 0, 0) == BST_CHECKED;
            DlssNrIniWriteInt(L"autoshow", now ? 1 : 0);
            LogRaw("ui: autoshow = %d (panel)", now ? 1 : 0);
            return 0;
        }
        if (id == IDC_DLS_STYLE  && code == CBN_SELCHANGE) { DlssNrPush(hwnd, true); return 0; }
        break;
    }
    case WM_CLOSE:
        if (s && s->standalone) { DestroyWindow(hwnd); return 0; }
        break;
    case WM_DESTROY:
        if (s) {
            if (s->font)       DeleteObject(s->font);
            if (s->fontBig  && s->fontBig  != s->font) DeleteObject(s->fontBig);
            if (s->fontSmall&& s->fontSmall!= s->font) DeleteObject(s->fontSmall);
        }
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static HWND DlssNrCreateSettings(HWND parent, bool standalone) {
    static bool classReady = false;
    if (!classReady) {
        WNDCLASSEXW wc = { sizeof(wc) };
        wc.lpfnWndProc   = DlssNrProc;
        wc.hInstance     = g_hModule;
        wc.hCursor       = LoadCursorW(nullptr, (LPCWSTR)IDC_ARROW);
        wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
        wc.lpszClassName = L"DlssNrSettingsWnd";
        if (!RegisterClassExW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
            return nullptr;
        classReady = true;
    }
    DlssNrSettings* s = (DlssNrSettings*)calloc(1, sizeof(DlssNrSettings));
    if (!s) return nullptr;
    s->standalone = standalone;

    int cx = DlssNrWantWidth(), cy = DlssNrWantHeight();
    DWORD style = standalone ? (WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX) : WS_CHILD;
    RECT rc = { 0, 0, cx, cy };
    if (standalone) AdjustWindowRect(&rc, style, FALSE);

    HWND h = CreateWindowExW(0, L"DlssNrSettingsWnd",
                             standalone ? L"DLSSNR" : L"", style,
                             CW_USEDEFAULT, CW_USEDEFAULT,
                             rc.right - rc.left, rc.bottom - rc.top,
                             parent, nullptr, g_hModule, s);
    if (!h) { free(s); return nullptr; }
    if (!standalone) SetWindowPos(h, nullptr, 0, 0, cx, cy, SWP_NOZORDER|SWP_NOACTIVATE);
    else { ShowWindow(h, SW_SHOW); UpdateWindow(h); }
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
    IDM_DLS_STYLE    = 100,     // +0..3
    IDM_DLS_INT      = 200,     // +0..4
    IDM_DLS_TONE     = 300,     // +0..4
    IDM_DLS_STRUCT   = 400,     // +0..4
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

    void AppendPctMenu(HMENU parent, const wchar_t* title, int baseId, float cur) {
        HMENU sub = CreatePopupMenu();
        int curPct = (int)(cur * 100.0f + 0.5f);
        for (int i = 0; i < kDlssNrPctCount; ++i) {
            wchar_t t[16];
            _snwprintf_s(t, _countof(t), _TRUNCATE, L"%d%%", kDlssNrPct[i]);
            UINT f = MF_STRING | (kDlssNrPct[i] == curPct ? MF_CHECKED : 0);
            AppendMenuW(sub, f, (UINT_PTR)(baseId + i), t);
        }
        AppendMenuW(parent, MF_POPUP, (UINT_PTR)sub, title);
    }

    void ShowMenu() {
        Engine& e = Engine::Instance();
        HMENU m = CreatePopupMenu();

        AppendMenuW(m, MF_STRING | (e.OptEnabled() ? MF_CHECKED : 0),
                    IDM_DLS_ENABLE, T(L"启用 DLSSNR", L"Enable DLSSNR"));

        static const wchar_t* zhStyle[4] = { L"默认", L"自然", L"电影", L"风格3" };
        static const wchar_t* enStyle[4] = { L"Default", L"Natural", L"Cinema", L"Style 3" };
        HMENU st = CreatePopupMenu();
        for (int i = 0; i < kDlssNrStyleCount; ++i)
            AppendMenuW(st, MF_STRING | (e.OptStyle() == i ? MF_CHECKED : 0),
                        (UINT_PTR)(IDM_DLS_STYLE + i),
                        (g_uiLang == 1 ? enStyle : zhStyle)[i]);
        AppendMenuW(m, MF_POPUP, (UINT_PTR)st, T(L"风格", L"Style"));
        AppendPctMenu(m, T(L"强度", L"Intensity"),        IDM_DLS_INT,    e.OptIntensity());
        AppendPctMenu(m, T(L"局部色调", L"Local tone"),    IDM_DLS_TONE,   e.OptTone());
        AppendPctMenu(m, T(L"局部结构", L"Local structure"), IDM_DLS_STRUCT, e.OptStruct());

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
        if (cmd == IDM_DLS_ENABLE)  { DlssNrApply(!e.OptEnabled(), e.OptStyle(), Pct(e.OptIntensity()), Pct(e.OptTone()), Pct(e.OptStruct()), true); UpdateTip(); return; }
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
            DlssNrApply(e.OptEnabled(), cmd - IDM_DLS_STYLE, Pct(e.OptIntensity()), Pct(e.OptTone()), Pct(e.OptStruct()), true);
            return;
        }
        struct { int base; const wchar_t* key; float cur; int idx; } grp[3] = {
            { IDM_DLS_INT,    L"intensity",   e.OptIntensity(), 0 },
            { IDM_DLS_TONE,   L"localtone",   e.OptTone(),      1 },
            { IDM_DLS_STRUCT, L"localstruct", e.OptStruct(),    2 },
        };
        for (auto& g : grp) {
            if (cmd >= g.base && cmd < g.base + kDlssNrPctCount) {
                int pct[3] = { Pct(e.OptIntensity()), Pct(e.OptTone()), Pct(e.OptStruct()) };
                pct[g.idx] = kDlssNrPct[cmd - g.base];
                DlssNrApply(e.OptEnabled(), e.OptStyle(), pct[0], pct[1], pct[2], true);
                return;
            }
        }
    }

    static int Pct(float v) {
        int p = (int)(v * 100.0f + 0.5f);
        return p < 0 ? 0 : (p > 100 ? 100 : p);
    }

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