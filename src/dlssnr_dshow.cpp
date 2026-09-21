// ===========================================================================
// dlssnr_dshow.cpp -- DirectShow transform filter wrapping the DLSSNR engine
//
// Copyright (c) 2026 Cyanke. MIT License -- see LICENSE at the repository root.
// Original work: not derived from purkatyy/DLSS5-. Contains no NVIDIA code; it
// loads the engine host (dlssnr_host2.dll) dynamically at runtime and degrades
// to pass-through when that host is absent.
//
// The Windows SDK ships strmbase.lib but NOT streams.h, so CTransformFilter and
// friends are unavailable (and there is no ATL). This filter is therefore fully
// self-contained: it implements IPin / IMemInputPin / IBaseFilter directly and
// reuses only the stock CLSID_MemoryAllocator for sample allocation.
//
// Why the engine drops in cleanly: dlssnr2_process() takes tightly packed
// BGR24 in system memory. DirectShow's MEDIASUBTYPE_RGB24 is B,G,R in memory
// (the Windows DIB convention -- the name lies, exactly like x2rgb10le did), so
// it matches the engine's contract byte for byte with no channel swap.
//
// Design notes that matter:
//   * Async init. Loading the network costs ~13 s. Blocking Connect() for that
//     long would freeze the player, so init runs on a worker thread and every
//     frame passes through untouched until the engine reports ready.
//   * Never break playback. Missing DLL, init failure, unsupported size, or a
//     process error all degrade to pass-through instead of failing the graph.
//   * Reset on seek. DLSSNR carries temporal history (measured: stale history
//     washes out within ~1 frame), so BeginFlush / discontinuity sets reset=1.
//
// ===========================================================================
#define WIN32_LEAN_AND_MEAN
#define _CRT_SECURE_NO_WARNINGS
#include <windows.h>
#include <dshow.h>
#include <strmif.h>
#include <amvideo.h>
#include <dvdmedia.h>      // VIDEOINFOHEADER2 / FORMAT_VideoInfo2
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include "dlssnr_engine.h"          // the built-in GPU engine
#if defined(_M_X64) || defined(_M_IX86)
#include <intrin.h>      // __cpuid
#include <tmmintrin.h>   // SSSE3 pshufb, for the vectorised colour conversion
#endif

// ---------------------------------------------------------------------------
// our filter CLSID: {3A44CEB0-B660-4131-A965-0EE8BA547C26}
// Defined as a plain static GUID rather than DEFINE_GUID so this file needs no
// INITGUID and cannot collide with the standard GUIDs that strmiids.lib owns.
// ---------------------------------------------------------------------------
static const GUID CLSID_DlssNrFilter =
{ 0x3a44ceb0, 0xb660, 0x4131, { 0xa9, 0x65, 0x0e, 0xe8, 0xba, 0x54, 0x7c, 0x26 } };

// Property page CLSID: {7E4B1C2A-5D3F-4A18-9C6E-2B8F0D51A734}
// Its own CLSID because the player's property frame creates the page itself,
// via CoCreateInstance, once our filter hands it over from GetPages().
static const GUID CLSID_DlssNrPage =
{ 0x7e4b1c2a, 0x5d3f, 0x4a18, { 0x9c, 0x6e, 0x2b, 0x8f, 0x0d, 0x51, 0xa7, 0x34 } };

static const wchar_t* kFilterName = L"DLSS Neural Render (DLSSNR)";
static const wchar_t* kIniName    = L"dlssnr_dshow.ini";
static const wchar_t* kLogName    = L"dlssnr_dshow.log";
static const wchar_t* kHostDll    = L"dlssnr_host2.dll";

// I420 is not a well-known GUID in uuids.h (only IYUV/YV12/NV12 are), but it is
// the conventional FOURCC-based subtype, so define it the standard way.
static const GUID MEDIASUBTYPE_I420_G =
{ 0x30323449, 0x0000, 0x0010, { 0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71 } };

// ---------------------------------------------------------------------------
// module dir + logging
// ---------------------------------------------------------------------------
static HMODULE       g_hModule = nullptr;
static wchar_t       g_dir[MAX_PATH] = L"";
static CRITICAL_SECTION g_logLock;
static bool          g_logLockInit = false;

static void LogRaw(const char* fmt, ...) {
    if (!g_logLockInit) return;
    EnterCriticalSection(&g_logLock);
    wchar_t path[MAX_PATH];
    _snwprintf_s(path, _countof(path), _TRUNCATE, L"%s\\%s", g_dir, kLogName);

    // Keep the log bounded: a player may hold this DLL for days. Rotate once
    // past the cap so the file stays inspectable instead of growing forever.
    static const LONGLONG kMaxBytes = 4 * 1024 * 1024;
    WIN32_FILE_ATTRIBUTE_DATA fad;
    if (GetFileAttributesExW(path, GetFileExInfoStandard, &fad)) {
        LONGLONG sz = ((LONGLONG)fad.nFileSizeHigh << 32) | fad.nFileSizeLow;
        if (sz > kMaxBytes) {
            wchar_t old[MAX_PATH];
            _snwprintf_s(old, _countof(old), _TRUNCATE, L"%s.old", path);
            DeleteFileW(old);
            MoveFileW(path, old);
        }
    }

    FILE* f = nullptr;
    _wfopen_s(&f, path, L"a");       // plain "a": ", ccs=UTF-8" + vfprintf crashes
    if (f) {
        SYSTEMTIME st; GetLocalTime(&st);
        fprintf(f, "[%02d:%02d:%02d.%03d] ", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
        va_list ap; va_start(ap, fmt);
        vfprintf(f, fmt, ap);
        va_end(ap);
        fprintf(f, "\n");
        fclose(f);
    }
    LeaveCriticalSection(&g_logLock);
}

// ---------------------------------------------------------------------------
// shared-memory control channel
//
// Answers two questions at once:
//   "is the filter actually loaded and running?"  -> the filter publishes
//      counters + status here, and the panel shows a live heartbeat.
//   "can I tune params while watching?"           -> the panel writes requested
//      values here and the filter applies them to the NEXT frame.
//
// All four DLSSNR parameters were measured to take effect mid-session without
// re-creating the feature, so no restart is needed when they change.
// ---------------------------------------------------------------------------
static const wchar_t* kSharedName = L"Local\\DLSSNR_DShow_Shared_v1";
static const unsigned int kSharedMagic = 0x4E534C44;   // 'DLSN'

#pragma pack(push, 1)
struct SharedState {
    // ---- header ----
    unsigned int magic;
    unsigned int version;
    unsigned int structSize;
    unsigned int pad0;

    // ---- published BY THE FILTER (status / telemetry) ----
    volatile long heartbeat;        // bumped every frame; proves it is loaded
    volatile long framesSeen;       // Receive() calls that produced output
    volatile long framesProcessed;  // frames the engine actually ran on
    volatile long framesPassthrough;
    volatile int  engineReady;      // 1 once the model finished loading
    volatile int  engineGaveUp;     // 1 if init failed permanently
    volatile int  engineW, engineH; // engine session size
    volatile int  videoW, videoH;   // current video geometry
    volatile int  inputBpp;         // 3 = BGR24 (fast path), 4 = BGR32
    volatile int  enabled;          // master switch currently in effect
    volatile float lastProcessMs;   // per-frame engine cost (EMA)
    volatile long pid;              // owning process id
    volatile int  lastReason;       // why pass-through (see enum below)
    volatile int  reqApplied;       // last reqSeq the filter has applied
    wchar_t status[128];            // human-readable one-liner

    // ---- written BY THE PANEL (requested parameters) ----
    // The panel bumps reqSeq after writing; the filter applies and echoes into
    // reqApplied, so the panel can confirm the change actually landed.
    volatile long  reqSeq;
    volatile int   reqEnabled;
    volatile int   reqStyle;
    volatile float reqIntensity;    // 0..1
    volatile float reqLocalTone;    // 0..1
    volatile float reqLocalStruct;  // 0..1
    volatile float reqMix;          // reserved
};
#pragma pack(pop)

// pass-through reason codes (mirrored by the panel)
enum {
    kReasonNone = 0,
    kReasonNotReady,      // still loading
    kReasonDisabled,      // enabled=0
    kReasonBpp,           // input is not BGR24
    kReasonSizeMismatch,  // geometry differs from the engine session
    kReasonProcessFail,   // engine returned an error
    kReasonGiveUp,        // init failed permanently
};

static HANDLE      g_map = nullptr;
static SharedState* g_shared = nullptr;

static void OpenShared() {
    if (g_shared) return;
    // Create the mapping. If the panel (or another filter instance) already made
    // it, we simply attach to the existing block.
    // NOTE the argument order: (hFile, attrs, protect, maxSizeHIGH, maxSizeLOW,
    // name). Passing the size in the HIGH slot asks for size*2^32 bytes, which
    // fails with ERROR_NO_SYSTEM_RESOURCES (1455).
    const DWORD size = (DWORD)sizeof(SharedState);
    g_map = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE,
                               0, size, kSharedName);
    if (!g_map) { LogRaw("shared: CreateFileMapping failed err=%lu", GetLastError()); return; }
    void* p = MapViewOfFile(g_map, FILE_MAP_ALL_ACCESS, 0, 0, size);
    if (!p) { LogRaw("shared: MapViewOfFile failed err=%lu", GetLastError()); CloseHandle(g_map); g_map = nullptr; return; }
    g_shared = (SharedState*)p;

    // Initialise only if we are the creator (magic not already valid).
    if (g_shared->magic != kSharedMagic) {
        ZeroMemory((void*)g_shared, sizeof(SharedState));
        g_shared->magic = kSharedMagic;
        g_shared->version = 1;
        g_shared->structSize = (unsigned int)sizeof(SharedState);
        // sensible defaults for the requested block
        g_shared->reqEnabled = 1;
        g_shared->reqStyle = 0;
        g_shared->reqIntensity = 1.0f;
        g_shared->reqLocalTone = 1.0f;
        g_shared->reqLocalStruct = 1.0f;
        g_shared->reqSeq = 0;
    }
    g_shared->pid = (long)GetCurrentProcessId();
    LogRaw("shared: attached (%u bytes)", (unsigned)sizeof(SharedState));
}

static void CloseShared() {
    if (g_shared) { UnmapViewOfFile((void*)g_shared); g_shared = nullptr; }
    if (g_map) { CloseHandle(g_map); g_map = nullptr; }
}

static void SetStatus(int reason, const char* fmt, ...) {
    if (!g_shared) return;
    g_shared->lastReason = reason;
    va_list ap; va_start(ap, fmt);
    char buf[128];
    _vsnprintf_s(buf, sizeof(buf), _TRUNCATE, fmt, ap);
    va_end(ap);
    MultiByteToWideChar(CP_UTF8, 0, buf, -1, g_shared->status, 128);
    g_shared->status[127] = 0;
}

// ---------------------------------------------------------------------------
// interface-probe logging
//
// When a player silently refuses to use a filter, the question that matters is
// "what did it ASK for?". QueryInterface probes are the answer, so log each
// distinct IID once per process (deduplicated to keep the log readable).
// ---------------------------------------------------------------------------
static void GuidToStr(const GUID& g, char* out, size_t n) {
    _snprintf_s(out, n, _TRUNCATE,
                "%08lX-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X",
                (unsigned long)g.Data1, g.Data2, g.Data3,
                g.Data4[0], g.Data4[1], g.Data4[2], g.Data4[3],
                g.Data4[4], g.Data4[5], g.Data4[6], g.Data4[7]);
}

// Returns a human name for the IIDs a player is likely to probe, so the log is
// readable without a GUID table.
static const char* KnownIidName(const GUID& g) {
    if (g == IID_IUnknown)          return "IUnknown";
    if (g == IID_IPin)              return "IPin";
    if (g == IID_IMemInputPin)      return "IMemInputPin";
    if (g == IID_IBaseFilter)       return "IBaseFilter";
    if (g == IID_IMediaFilter)      return "IMediaFilter";
    if (g == IID_IPersist)          return "IPersist";
    if (g == IID_IEnumPins)         return "IEnumPins";
    if (g == IID_IEnumMediaTypes)   return "IEnumMediaTypes";
    if (g == IID_IMemAllocator)     return "IMemAllocator";
    if (g == IID_IMediaSample)      return "IMediaSample";
    if (g == IID_IQualityControl)   return "IQualityControl";
    if (g == IID_IAMStreamConfig)   return "IAMStreamConfig";
    if (g == IID_IKsPropertySet)    return "IKsPropertySet";
    if (g == IID_ISpecifyPropertyPages) return "ISpecifyPropertyPages";
    if (g == IID_IMediaPosition)    return "IMediaPosition";
    if (g == IID_IMediaSeeking)     return "IMediaSeeking";
    if (g == IID_IVideoWindow)      return "IVideoWindow";
    if (g == IID_IBasicVideo)       return "IBasicVideo";
    if (g == IID_IPersistStream)    return "IPersistStream";
    if (g == IID_IAMFilterMiscFlags) return "IAMFilterMiscFlags";
    return "(unnamed)";
}

// Logs the first N distinct probes; beyond that it stays quiet so a player that
// polls in a loop cannot flood the file.
static void LogProbeOnce(const wchar_t* who, const GUID& iid, bool supported) {
    // narrow the (ASCII) pin name for the logger
    char whoA[64];
    WideCharToMultiByte(CP_UTF8, 0, who, -1, whoA, sizeof(whoA), nullptr, nullptr);

    static GUID seen[64];
    static int  seenN = 0;
    static CRITICAL_SECTION lk;
    static bool init = false;
    if (!init) { InitializeCriticalSection(&lk); init = true; }

    EnterCriticalSection(&lk);
    for (int i = 0; i < seenN; ++i) {
        if (memcmp(&seen[i], &iid, sizeof(GUID)) == 0) { LeaveCriticalSection(&lk); return; }
    }
    if (seenN < 64) seen[seenN++] = iid;
    LeaveCriticalSection(&lk);

    char s[64];
    GuidToStr(iid, s, sizeof(s));
    const char* nm = KnownIidName(iid);
    if (strcmp(nm, "(unnamed)") == 0)
        LogRaw("PROBE %s QI {%s} -> %s", whoA, s, supported ? "OK" : "E_NOINTERFACE");
    else
        LogRaw("PROBE %s QI %s -> %s", whoA, nm, supported ? "OK" : "E_NOINTERFACE");
}

// Overload so plain char* call sites keep working.
static void LogProbeOnce(const char* who, const GUID& iid, bool supported) {
    wchar_t w[64];
    MultiByteToWideChar(CP_UTF8, 0, who, -1, w, 64);
    LogProbeOnce(w, iid, supported);
}

// ---------------------------------------------------------------------------
// SHELVED PAUSED-REFRESH ATTEMPTS (see RefreshIfNeeded)

//
// The path a player's own frame-step key takes: the graph manager runs the graph,
// WAITS for exactly one frame to be presented, then pauses again. That wait is the
// part that matters -- an immediate Run/Pause transition can complete without the
// source ever handing over a new frame, which is why it showed nothing.
//
// Field log (PotPlayer): CanStep 0x00000000, Step 0x00000000, and a frame arrives
// while paused. The cost is that the video advances by exactly one frame.
//
// Declared locally (with its published IID) so the build needs no extra import
// library.
// ---------------------------------------------------------------------------
// ---------------------------------------------------------------------------
// Refresh hook
//
// The panel/tray applies a parameter change through DlssNrApply(), which then tells
// every live filter instance that an edit happened. On the PLAYER's side this cannot
// repaint a paused picture (every mechanism was measured; see RefreshIfNeeded()), but
// it is exactly what the panel's own 对比 (compare) view needs: there the engine
// re-runs the frame it kept and the panel repaints from a buffer, touching nothing of
// the player's.
//
// The filter class is defined below the UI include, hence a plain callback registry
// instead of a class reference.
// ---------------------------------------------------------------------------
typedef void (*DlssNrRefreshFn)(void*, bool committed);
struct DlssNrRefreshSink {
    DlssNrRefreshFn fn;
    void*           ctx;
};
static DlssNrRefreshSink g_refreshSinks[8] = {};
static CRITICAL_SECTION  g_refreshLock;

// ---- comparison preview buffer (read by the panel's 对比 window) ----------
//
// Two packed BGR24, top-down images of the same frame: as the graph delivered it, and
// as the engine renders it. DirectShow's RGB24 is already B,G,R in memory, so the
// panel can hand the buffer straight to StretchDIBits. Guarded, because the filter
// fills it on the player's streaming thread while the panel paints it.
static CRITICAL_SECTION g_prevCs;
static bool   g_prevOn = false;      // the compare window is open
static int    g_prevW = 0, g_prevH = 0;
static LONG   g_prevSeq = 0;         // bumped whenever the images change
static size_t g_prevCap = 0;         // bytes per image
static BYTE*  g_prevBuf = nullptr;   // [0,cap) original, [cap,2*cap) processed
// The compare view refreshes by re-running the engine on the frame it kept.
// That frame is kept as BGR24 (the panel wants DIB order), so the refresh makes
// a round trip BGR24 -> NV12 -> engine -> NV12 -> BGR24, needing its own pair of
// NV12 buffers (a conversion cannot run in place).
static BYTE*  g_prevNv12    = nullptr;
static size_t g_prevNv12Cap = 0;      // bytes per NV12 image

static bool     DlssNrPreviewWant()    { return g_prevOn; }
static void     DlssNrPreviewSetWant(bool on) { g_prevOn = on; }
static LONG     DlssNrPreviewSeq()     { return g_prevSeq; }
static int      DlssNrPreviewW()       { return g_prevW; }
static int      DlssNrPreviewH()       { return g_prevH; }
static const BYTE* DlssNrPreviewOrig() { return g_prevBuf; }
static const BYTE* DlssNrPreviewProc() { return g_prevBuf ? g_prevBuf + g_prevCap : nullptr; }
static void     DlssNrPreviewLock()    { EnterCriticalSection(&g_prevCs); }
static void     DlssNrPreviewUnlock()  { LeaveCriticalSection(&g_prevCs); }

// Bumped by the filter after re-rendering the kept frame with new parameters.
static void DlssNrPreviewTouch() { InterlockedIncrement(&g_prevSeq); }

static void DlssNrAddRefreshSink(DlssNrRefreshFn fn, void* ctx) {
    EnterCriticalSection(&g_refreshLock);
    bool added = false;
    for (DlssNrRefreshSink& s : g_refreshSinks) {
        if (!s.fn) { s.fn = fn; s.ctx = ctx; added = true; break; }
    }
    LeaveCriticalSection(&g_refreshLock);
    if (!added) LogRaw("refresh: sink table full, the panel cannot repaint paused frames");
}

static void DlssNrRemoveRefreshSink(DlssNrRefreshFn fn, void* ctx) {
    EnterCriticalSection(&g_refreshLock);
    for (DlssNrRefreshSink& s : g_refreshSinks) {
        if (s.fn == fn && s.ctx == ctx) { s.fn = nullptr; s.ctx = nullptr; break; }
    }
    LeaveCriticalSection(&g_refreshLock);
}

// Called by the panel/tray the moment new live options were applied.
static void DlssNrRefreshSinks(bool committed) {
    DlssNrRefreshSink local[8];
    int n = 0;
    EnterCriticalSection(&g_refreshLock);
    for (const DlssNrRefreshSink& s : g_refreshSinks) if (s.fn && n < 8) local[n++] = s;
    LeaveCriticalSection(&g_refreshLock);
    for (int i = 0; i < n; ++i) local[i].fn(local[i].ctx, committed);
}

static void InitModuleDir(HMODULE h) {
    g_hModule = h;
    GetModuleFileNameW(h, g_dir, _countof(g_dir));
    wchar_t* slash = wcsrchr(g_dir, L'\\');
    if (slash) *slash = 0;
    InitializeCriticalSection(&g_logLock);
    g_logLockInit = true;
    InitializeCriticalSection(&g_refreshLock);   // refresh sink registry
    InitializeCriticalSection(&g_prevCs);        // comparison preview buffer
    // Attach AFTER the log lock exists: OpenShared() logs on failure.
    OpenShared();
}

// ---------------------------------------------------------------------------
// configuration (dlssnr_dshow.ini next to the DLL; absent file = defaults)
// ---------------------------------------------------------------------------
struct Config {
    bool  enabled;
    int   style;
    float intensity;     // 0..1
    float localTone;     // 0..1
    float localStruct;   // 0..1
    float skinStruct;    // 0..2, 0 = off (same range/default as Magpie's
                         //   DLSSNR "Skin Structure Strength")
    bool  autoMask;      // DLSSNR.UseAutoMask
};

static Config ReadConfig() {
    Config c;
    c.enabled     = true;
    c.style       = 0;
    c.intensity   = 1.0f;
    c.localTone   = 1.0f;
    c.localStruct = 1.0f;
    c.skinStruct  = 0.0f;
    c.autoMask    = false;

    wchar_t ini[MAX_PATH];
    _snwprintf_s(ini, _countof(ini), _TRUNCATE, L"%s\\%s", g_dir, kIniName);
    if (GetFileAttributesW(ini) == INVALID_FILE_ATTRIBUTES) return c;

    c.enabled     = GetPrivateProfileIntW(L"DLSSNR", L"enabled",     1, ini) != 0;
    c.style       = GetPrivateProfileIntW(L"DLSSNR", L"style",       0, ini);
    int iInt      = GetPrivateProfileIntW(L"DLSSNR", L"intensity", 100, ini);
    int iTone     = GetPrivateProfileIntW(L"DLSSNR", L"localtone",  100, ini);
    int iStruct   = GetPrivateProfileIntW(L"DLSSNR", L"localstruct",100, ini);
    int iSkin     = GetPrivateProfileIntW(L"DLSSNR", L"skinstructure", 0, ini);
    // 100% = 1.0 is the "full strength" default everywhere, so old ini files keep
    // their meaning. Only the parameters that were measured to keep responding
    // above 1.0 accept up to 200% = 2.0: DLSSNR.Intensity saturates at 1.0
    // (1.0 and 2.0 gave bit-identical frames), so it is capped at 100%.
    c.intensity   = (float)(iInt    < 0 ? 0 : (iInt    > 100 ? 100 : iInt))    / 100.0f;
    c.localTone   = (float)(iTone   < 0 ? 0 : (iTone   > 200 ? 200 : iTone))   / 100.0f;
    c.localStruct = (float)(iStruct < 0 ? 0 : (iStruct > 200 ? 200 : iStruct)) / 100.0f;
    c.skinStruct  = (float)(iSkin   < 0 ? 0 : (iSkin   > 200 ? 200 : iSkin))   / 100.0f;
    c.autoMask    = GetPrivateProfileIntW(L"DLSSNR", L"automask", 0, ini) != 0;
    return c;
}

// High-resolution monotonic milliseconds (used for the per-frame cost EMA).
static double NowMsLocal() {
    static LARGE_INTEGER freq{}; static bool init = false;
    if (!init) { QueryPerformanceFrequency(&freq); init = true; }
    LARGE_INTEGER c; QueryPerformanceCounter(&c);
    return (double)c.QuadPart * 1000.0 / (double)freq.QuadPart;
}

static float Clamp01(float v) {
    if (!(v == v)) return 1.0f;          // NaN guard: a bad panel write must not
    if (v < 0.0f) return 0.0f;           // poison the engine parameters
    if (v > 1.0f) return 1.0f;
    return v;
}

// Same guard for the parameter whose range is not 0..1 (skin structure is
// 0..2, matching Magpie's DLSSNR default of 0 = off).
static float ClampRange(float v, float lo, float hi, float fallback) {
    if (!(v == v)) return fallback;
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

// ---------------------------------------------------------------------------
// engine: built in (src/dlssnr_engine.cpp)
// ---------------------------------------------------------------------------
// This used to LoadLibrary("dlssnr_host2.dll") -- the engine host published by
// the separate dlssnr-toolkit repository -- and feed it CPU-converted BGR24.
// The engine now lives in this DLL and speaks NV12 end to end, so there is
// nothing to load and no NTSC-era colour conversion on the CPU.

class Engine {
public:
    // ---------------------------------------------------------------------
    // The engine is PROCESS-WIDE, not per filter instance.
    //
    // NGX can only be initialised once per process, and the host keeps that
    // latch inside its own DLL image (the static bool ngxInited in
    // dlssnr_host2.cpp). A player builds a NEW filter instance for every file
    // it opens, so the old per-instance engine meant: open another file ->
    // instance destroyed -> FreeLibrary(host) -> latch lost ->
    // NVSDK_NGX_D3D12_Init_Ext called a second time in the same process -> NGX
    // answers 0xBAD00002 (FAIL_PlatformError) -> 3 failed attempts -> permanent
    // passthrough until the player itself was restarted. One engine per process
    // is the only arrangement that matches NGX's lifetime rules.
    //
    // A resolution change still works: that path calls dlssnr::Init() again,
    // which reuses the D3D12 device and rebuilds only the NGX feature and the
    // frame resources (see src/dlssnr_engine.cpp).
    //
    // Deliberately leaked: it owns a worker thread whose code lives in this DLL,
    // so running its destructor while the module is being unloaded would execute
    // code in a module that is going away.
    static Engine& Instance() {
        static Engine* inst = CreatePinned();
        return *inst;
    }
    Engine() : m_ready(false), m_failed(false),
               m_w(0), m_h(0), m_thread(nullptr),
               m_evtWake(nullptr), m_quit(false) {
        InitializeCriticalSection(&m_lock);
        m_evtWake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        // The worker lives for the whole process, so RequestInit() never has to
        // create a thread (or take m_lock) while a graph thread waits on it.
        m_thread = CreateThread(nullptr, 0, &Engine::ThreadProc, this, 0, nullptr);
        if (!m_thread) LogRaw("engine: CreateThread failed");
    }
    ~Engine() {
        m_quit = true;
        if (m_evtWake) SetEvent(m_evtWake);
        if (m_thread) { WaitForSingleObject(m_thread, 30000); CloseHandle(m_thread); }
        if (m_evtWake) CloseHandle(m_evtWake);
        // The engine is in-process now, but the rule still holds in spirit: the NGX
    // core outlives every filter instance and must never be torn down, because
    // re-initialising it in one process is not safe. The original reason:
    // unloading the host would discard
        // its "NGX already inited" latch, and the next dlssnr2_init() would call
        // Init_Ext a second time and fail with 0xBAD00002 -- the exact bug the
        // process-wide Engine::Instance() above exists to prevent. Process exit
        // reclaims it, so the payload module simply stays loaded.
        DeleteCriticalSection(&m_lock);
    }

    // Called when a new filter instance starts (the player opened another file).
    // This engine now lives for the whole process, so the give-up latches from
    // the previous file must be cleared here; otherwise a single transient
    // failure would disable DLSSNR for every later file in the same process.
    //
    // MUST NOT block: a player thread calls this while building a graph, and the
    // engine thread can hold m_lock across a 1-15 s model load. If the lock is
    // busy the request is recorded and the worker applies it on its next lap.
    void NewSession() {
        InterlockedExchange(&m_sessionReset, 1);
        if (TryEnterCriticalSection(&m_lock)) {
            ApplySessionReset();
            LeaveCriticalSection(&m_lock);
        }
    }

    // Clears the per-file latches. Call with m_lock held.
    void ApplySessionReset() {
        m_gaveUp = false;
        m_tries = 0;
        m_tryW = m_tryH = 0;
        m_failed = false;
    }

    // Kick off (or re-target) a background init. MUST NOT block: this is called
    // from the player's graph thread (IMediaFilter::Pause during a seek), and
    // the engine thread holds m_lock across the whole dlssnr2_init() -- the
    // 1-15 s model load. Taking m_lock here froze PotPlayer for the length of
    // that load, which the user sees as "not responding" while seeking.
    void RequestInit(int w, int h) {
        InterlockedExchange(&m_wantW, (LONG)w);
        InterlockedExchange(&m_wantH, (LONG)h);
        if (m_evtWake) SetEvent(m_evtWake);
    }

    bool Ready()      { return m_ready; }
    bool Failed()     { return m_failed; }
    int  Width()      { return m_w; }
    int  Height()     { return m_h; }
    bool GaveUp()     { return m_gaveUp; }
    float LastProcessMs() { return (float)m_lastProcessMs; }
    // Bumped on every live option change. The filter polls this so a panel edit
    // made while the graph is paused (no frames flowing) can still be applied to
    // the cached frame and re-delivered.
    LONG ParameterRevision() { return m_paramRev; }

    // Current requested options -- read by the settings window and the tray
    // menu so they open showing the live state rather than the ini defaults.
    bool  OptEnabled()   { return m_optEnabled; }
    bool  OptSeeded()    { return m_seeded; }

    // The ini is only the STARTING point. Because this engine lives for the
    // whole process, re-reading the ini on every new filter instance would
    // revert a change the user just made in the control panel, so seed once.
    void SeedOptionsOnce(bool enabled, int style, float intensity, float tone,
                         float strct, float skin, bool autoMask) {
        EnterCriticalSection(&m_lock);
        bool first = !m_seeded;
        m_seeded = true;
        LeaveCriticalSection(&m_lock);
        if (first) SetOptions(enabled, style, intensity, tone, strct, skin, autoMask);
    }
    dlssnr::Options Opts() const {
        dlssnr::Options o;
        o.style = m_optStyle;
        o.intensity = m_optIntensity;
        o.localTone = m_optTone;
        o.localStruct = m_optStrct;
        o.skinStruct = m_optSkin;
        o.autoMask = m_optAutoMask ? 1 : 0;
        o.uiCorrection = 0;
        return o;
    }
    int   OptStyle()     { return m_optStyle; }
    float OptIntensity() { return m_optIntensity; }
    float OptTone()      { return m_optTone; }
    float OptStruct()    { return m_optStrct; }
    float OptSkin()      { return m_optSkin; }
    bool  OptAutoMask()  { return m_optAutoMask; }

    // Apply engine options. Safe to call when not initialised yet: the values
    // are cached in m_cfg and pushed again by DoInit(), so a panel change made
    // before the model loads still takes effect.
    void SetOptions(bool enabled, int style, float intensity, float tone,
                    float strct, float skin, bool autoMask) {
        m_optEnabled = enabled; m_optStyle = style; m_optIntensity = intensity;
        m_optTone = tone; m_optStrct = strct;
        // Defensive: a stale ini or a hand-edited config must not push an
        // out-of-range value into the NGX parameter block.
        m_optSkin = ClampRange(skin, 0.0f, 2.0f, 0.0f);
        m_optAutoMask = autoMask;
        InterlockedIncrement(&m_paramRev);
        m_optDirty = true;
        if (m_ready) {
            EnterCriticalSection(&m_lock);
            dlssnr::SetOptions(Opts());
            LeaveCriticalSection(&m_lock);
            m_optDirty = false;
        }
    }

    // Returns true and fills dst, or false to tell the caller to pass through.
    // MUST NOT block on the init lock: the worker holds it for the whole ~13 s
    // model load, and this runs on the player's streaming thread. If an init is
    // in flight we simply pass this frame through (TryEnterCriticalSection).
    // One NV12 frame in, one NV12 frame out. The colour conversion and the
    // neural pass both run on the GPU; all the CPU does is copy two planes.
    bool ProcessNv12(const BYTE* inY, int inYStride, const BYTE* inUV, int inUVStride,
                     BYTE* outY, int outYStride, BYTE* outUV, int outUVStride,
                     int w, int h, bool reset) {
        // Load the size fields once into locals; they are written under the lock
        // by the worker, so reading them twice could see torn values.
        const int readyW = m_w, readyH = m_h;
        if (!m_ready || readyW != w || readyH != h) return false;
        if (!TryEnterCriticalSection(&m_lock)) return false;   // init in progress
        // Re-check under the lock: m_ready may have flipped since the cheap test.
        double t0 = NowMsLocal();
        bool ok = false;
        if (m_ready && m_w == w && m_h == h) {
            ok = dlssnr::ProcessNv12(inY, inYStride, inUV, inUVStride,
                                     outY, outYStride, outUV, outUVStride, reset);
        }
        LeaveCriticalSection(&m_lock);
        if (!ok) { LogRaw("engine: process failed %dx%d -> passthrough", w, h); return false; }
        // EMA keeps the reported cost readable while still reacting to changes.
        const double dt = NowMsLocal() - t0;
        m_lastProcessMs = (m_lastProcessMs <= 0.0) ? dt : (m_lastProcessMs * 0.9 + dt * 0.1);
        return true;
    }

private:
    static DWORD WINAPI ThreadProc(LPVOID self) { ((Engine*)self)->Run(); return 0; }

    // Pin this DLL for the rest of the process before creating the singleton.
    // The engine outlives every filter instance and owns a thread that runs
    // inside this module, so the module must never be unloaded while it lives.
    static Engine* CreatePinned() {
        HMODULE self = nullptr;
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_PIN |
                           GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
                           (LPCWSTR)&Engine::CreatePinned, &self);
        return new Engine();
    }


    void Run() {
        for (;;) {
            if (m_quit) return;
            int w, h;
            EnterCriticalSection(&m_lock);
            if (InterlockedExchange(&m_sessionReset, 0)) ApplySessionReset();
            w = (int)m_wantW; h = (int)m_wantH;
            bool needInit = (w > 0 && h > 0) && !(m_ready && m_w == w && m_h == h);
            LeaveCriticalSection(&m_lock);

            if (needInit) {
                // A failed init must NOT be retried in a hot loop: the old code
                // spun here thousands of times a second, pegging a core and
                // flooding the log. After kMaxTries attempts for one geometry,
                // stop until the geometry changes or another file is opened
                // (Engine::NewSession clears the latch).
                if (m_tryW == w && m_tryH == h && m_tries >= kMaxTries) {
                    if (!m_gaveUp) {
                        m_gaveUp = true;
                        LogRaw("engine: giving up on %dx%d after %d attempts "
                               "(engine init keeps failing - DLSSNR stays in "
                               "passthrough; see the host engine log lines above)",
                               w, h, m_tries);
                    }
                    WaitForSingleObject(m_evtWake, 500);
                    continue;
                }
                if (m_tryW != w || m_tryH != h) { m_tryW = w; m_tryH = h; m_tries = 0; m_gaveUp = false; }
                ++m_tries;
                DoInit(w, h);
                // breathe between attempts so a persistent failure stays cheap
                if (!m_ready) Sleep(250);
            } else {
                WaitForSingleObject(m_evtWake, 200);
            }
        }
    }

    void DoInit(int w, int h) {
        EnterCriticalSection(&m_lock);
        m_ready = false;
        LeaveCriticalSection(&m_lock);

        Config cfg = ReadConfig();

        char log[MAX_PATH];
        _snprintf_s(log, sizeof(log), _TRUNCATE, "%ls\\%ls", g_dir, kLogName);
        LogRaw("engine: init %dx%d (model load may take a few seconds)...", w, h);

        // The whole init must hold m_lock: dlssnr2_init() releases and recreates
        // the D3D12 feature + textures, while the player's streaming thread may
        // be inside dlssnr2_process() on the very same objects. Without this the
        // two race and that is a use-after-free inside the engine.
        EnterCriticalSection(&m_lock);
        // Prefer the cached options: if the panel changed something while the
        // model was still loading, that choice must survive the init rather than
        // being overwritten by the INI values read at construction time.
        if (!m_optDirty) {
            m_optStyle = cfg.style; m_optIntensity = cfg.intensity;
            m_optTone = cfg.localTone; m_optStrct = cfg.localStruct;
            m_optSkin = cfg.skinStruct; m_optAutoMask = cfg.autoMask;
            m_optEnabled = cfg.enabled;
        }
        dlssnr::SetOptions(Opts());
        m_optDirty = false;
        const bool ok = dlssnr::Init(w, h, g_dir, log);
        if (ok) {
            m_w = dlssnr::Width();
            m_h = dlssnr::Height();
            m_ready = true;
            m_failed = false;
        }
        LeaveCriticalSection(&m_lock);

        if (!ok) {
            LogRaw("engine: init FAILED %dx%d -> staying in passthrough", w, h);
            return;   // keep thread alive; a later type change may succeed
        }
        LogRaw("engine: READY %dx%d", m_w, m_h);
        // The UI is the tray icon, which owns its own thread and opens the
        // native control panel; nothing to launch from here (and no Python).
    }

    volatile bool m_ready;
    volatile bool m_failed;
    int          m_w, m_h;
    HANDLE       m_thread;
    // Requested geometry. Written from graph threads with InterlockedExchange
    // (never under m_lock) and read by the worker; aligned 32-bit accesses.
    volatile LONG m_wantW = 0, m_wantH = 0;
    HANDLE       m_evtWake;
    volatile bool m_quit;
    CRITICAL_SECTION m_lock;
    // init-attempt bookkeeping (guards against a hot retry loop on failure)
    static const int kMaxTries = 3;
    int  m_tryW = 0, m_tryH = 0, m_tries = 0;
    bool m_gaveUp = false;
    // Set by NewSession() (a player thread) and consumed by the worker, which
    // owns the actual bookkeeping fields.
    volatile LONG m_sessionReset = 0;
    // engine options (kept here so a panel change before init still applies)
    bool  m_optEnabled = true;
    bool  m_seeded     = false;
    int   m_optStyle = 0;
    float m_optIntensity = 1.0f, m_optTone = 1.0f, m_optStrct = 1.0f;
    float m_optSkin = 0.0f;              // 0..2 (0 = off)
    bool  m_optAutoMask = false;
    volatile bool m_optDirty = false;
    volatile double m_lastProcessMs = 0.0;
    volatile LONG m_paramRev = 0;
};

// The native UI (tray icon + settings window) and the DirectShow property page.
// Both are ours and are compiled into this same translation unit.
#include "dlssnr_ui.h"
#include "dlssnr_page.h"

// ---------------------------------------------------------------------------
// media-type helpers
//
// The filter accepts BOTH packed RGB and planar YUV 4:2:0. YUV matters because
// players overwhelmingly decode to NV12 (hardware decoders almost always do),
// and a filter that only takes RGB24 simply never gets inserted.
//
// It also accepts BOTH FORMAT_VideoInfo and FORMAT_VideoInfo2. Decoders and
// renderers commonly negotiate with VideoInfo2 (it carries interlacing / aspect
// info), and rejecting it means the graph is never built -- the filter gets
// created and then never receives a single frame.
//
// RGB24 is accepted as the engine's native layout: MEDIASUBTYPE_RGB24 is B,G,R
// in memory (the Windows DIB convention -- the name lies, like x2rgb10le did).
// YUV is converted to BGR24 in the staging buffer, so the engine only ever sees
// its native format.
// ---------------------------------------------------------------------------
static bool IsYuvType(const AM_MEDIA_TYPE* pmt) {
    if (!pmt) return false;
    return pmt->subtype == MEDIASUBTYPE_NV12 ||
           pmt->subtype == MEDIASUBTYPE_YV12 ||
           pmt->subtype == MEDIASUBTYPE_IYUV ||
           pmt->subtype == MEDIASUBTYPE_I420_G;
}

static bool IsVideoInfoFormat(const AM_MEDIA_TYPE* pmt) {
    if (!pmt) return false;
    if (pmt->formattype == FORMAT_VideoInfo)
        return pmt->pbFormat && pmt->cbFormat >= sizeof(VIDEOINFOHEADER);
    if (pmt->formattype == FORMAT_VideoInfo2)
        return pmt->pbFormat && pmt->cbFormat >= sizeof(VIDEOINFOHEADER2);
    return false;
}

// Points at the BITMAPINFOHEADER inside either format block.
static BITMAPINFOHEADER* BitmapHeaderOf(AM_MEDIA_TYPE* pmt) {
    if (!pmt || !pmt->pbFormat) return nullptr;
    if (pmt->formattype == FORMAT_VideoInfo && pmt->cbFormat >= sizeof(VIDEOINFOHEADER))
        return &((VIDEOINFOHEADER*)pmt->pbFormat)->bmiHeader;
    if (pmt->formattype == FORMAT_VideoInfo2 && pmt->cbFormat >= sizeof(VIDEOINFOHEADER2))
        return &((VIDEOINFOHEADER2*)pmt->pbFormat)->bmiHeader;
    return nullptr;
}
static const BITMAPINFOHEADER* BitmapHeaderOf(const AM_MEDIA_TYPE* pmt) {
    return BitmapHeaderOf(const_cast<AM_MEDIA_TYPE*>(pmt));
}

static bool IsSupportedType(const AM_MEDIA_TYPE* pmt) {
    if (!pmt) return false;
    if (pmt->majortype != MEDIATYPE_Video) return false;
    if (!IsVideoInfoFormat(pmt)) return false;
    if (pmt->subtype == MEDIASUBTYPE_RGB24) return true;
    if (pmt->subtype == MEDIASUBTYPE_RGB32) return true;
    if (IsYuvType(pmt)) return true;
    return false;
}

static int BytesPerPixel(const AM_MEDIA_TYPE* pmt) {
    // For planar YUV this is a nominal value; the real layout is handled by the
    // conversion path. Kept because the RGB paths call it.
    if (IsYuvType(pmt)) return 1;
    return (pmt->subtype == MEDIASUBTYPE_RGB32) ? 4 : 3;
}

// DirectShow packs rows to a DWORD boundary regardless of the nominal width.
static int StrideOf(int w, int bpp) {
    return ((w * bpp + 3) & ~3);
}

// Buffer size for a media type. YUV 4:2:0 is width*height*3/2 (plus the DIB row
// padding on the luma plane only, which is how hardware decoders lay it out).
static long SampleSizeOf(const AM_MEDIA_TYPE* pmt, int w, int h) {
    if (IsYuvType(pmt)) return (long)StrideOf(w, 1) * h * 3 / 2;
    return (long)StrideOf(w, BytesPerPixel(pmt)) * h;
}

// ---------------------------------------------------------------------------
// YUV 4:2:0 -> BGR24 (BT.709 limited range, matching the reported "色彩空间" of
// typical HD sources; the coefficients keep the round-trip error under 3/255).
// ---------------------------------------------------------------------------
static inline BYTE Clip8(int v) { return (BYTE)(v < 0 ? 0 : (v > 255 ? 255 : v)); }

// ---------------------------------------------------------------------------
// NV12 -> packed BGR24.
//
// This is on the per-frame hot path, and the scalar version is expensive: at
// 3840x2076 it costs ~29 ms/frame, i.e. 0.82 GB/s of output - two orders of
// magnitude below memory bandwidth, so it is instruction-bound, not
// bandwidth-bound. The build is /O2 with no /arch: flag, so MSVC never
// vectorised it. The host DLL (dlssnr-toolkit) already vectorises its own
// BGR<->RGBA helpers with SSSE3, so this just brings the filter to the same
// standard. Measured ~13x faster, and BIT-EXACT with the scalar loop it
// replaces (verified byte-for-byte over odd geometries, 1x1, 3840x2076, and
// real 4K frames - see tools/simd_conv_nv12_to_bgr.cpp).
//
// Coefficients are BT.709 limited range, matching the "色彩空间" typical HD
// sources report; measured round-trip error <= 3/255.
// ---------------------------------------------------------------------------
static void Nv12ToBgr24_scalar(const BYTE* src, int w, int h, int strideY, int strideUV,
                               BYTE* dst) {
    for (int y = 0; y < h; ++y) {
        const BYTE* yrow = src + (size_t)y * strideY;
        const BYTE* uv = src + (size_t)strideY * h + (size_t)(y >> 1) * strideUV;
        BYTE* drow = dst + (size_t)y * w * 3;
        for (int x = 0; x < w; ++x) {
            const int Y = yrow[x];
            const int U = uv[(x >> 1) * 2];
            const int V = uv[(x >> 1) * 2 + 1];
            const int C = Y - 16;
            const int D = U - 128;
            const int E = V - 128;
            drow[x * 3 + 0] = Clip8((298 * C + 541 * D + 128) >> 8);           // B
            drow[x * 3 + 1] = Clip8((298 * C - 55 * D - 136 * E + 128) >> 8);  // G
            drow[x * 3 + 2] = Clip8((298 * C + 459 * E + 128) >> 8);           // R
        }
    }
}

#if defined(_M_X64) || defined(_M_IX86)
// Filled once in DllMain; DllMain runs before any pin exists.
static bool g_convSSSE3 = false;

// 8 pixels -> 8 channel bytes in the low half of the register. idx: 0=B 1=G 2=R.
static inline __m128i Nv12Bgr8(__m128i C, __m128i D, __m128i E, int idx) {
    const __m128i zero = _mm_setzero_si128();
    const __m128i k128 = _mm_set1_epi32(128);
    __m128i lo, hi, p;
    if (idx == 0) {                                    // B = (298C + 541D + 128)>>8
        const __m128i k = _mm_setr_epi16(298, 541, 298, 541, 298, 541, 298, 541);
        lo = _mm_madd_epi16(_mm_unpacklo_epi16(C, D), k);
        hi = _mm_madd_epi16(_mm_unpackhi_epi16(C, D), k);
    } else if (idx == 2) {                             // R = (298C + 459E + 128)>>8
        const __m128i k = _mm_setr_epi16(298, 459, 298, 459, 298, 459, 298, 459);
        lo = _mm_madd_epi16(_mm_unpacklo_epi16(C, E), k);
        hi = _mm_madd_epi16(_mm_unpackhi_epi16(C, E), k);
    } else {                                           // G = (298C - 55D - 136E + 128)>>8
        const __m128i kcd = _mm_setr_epi16(298, -55, 298, -55, 298, -55, 298, -55);
        const __m128i ke  = _mm_setr_epi16(-136, 0, -136, 0, -136, 0, -136, 0);
        lo = _mm_add_epi32(_mm_madd_epi16(_mm_unpacklo_epi16(C, D), kcd),
                           _mm_madd_epi16(_mm_unpacklo_epi16(E, zero), ke));
        hi = _mm_add_epi32(_mm_madd_epi16(_mm_unpackhi_epi16(C, D), kcd),
                           _mm_madd_epi16(_mm_unpackhi_epi16(E, zero), ke));
    }
    // >>8 is arithmetic: the scalar code shifts a signed int.
    lo = _mm_srai_epi32(_mm_add_epi32(lo, k128), 8);
    hi = _mm_srai_epi32(_mm_add_epi32(hi, k128), 8);
    p  = _mm_packs_epi32(lo, hi);                      // 8 x int16 (saturated)
    return _mm_packus_epi16(p, p);                     // 8 bytes, clipped 0..255 = Clip8
}

static void Nv12ToBgr24_simd(const BYTE* src, int w, int h, int strideY, int strideUV,
                             BYTE* dst) {
    const BYTE* uvbase = src + (size_t)strideY * h;
    const __m128i zero = _mm_setzero_si128();
    const __m128i c16  = _mm_set1_epi16(16);
    const __m128i c128 = _mm_set1_epi16(128);
    // bg = [B0 G0 B1 G1 ...]; splice R into every 3rd byte of the BGR stream.
    const __m128i m_bg0 = _mm_setr_epi8(0,1,-1,2,3,-1,4,5,-1,6,7,-1,8,9,-1,10);
    const __m128i m_r0  = _mm_setr_epi8(-1,-1,0,-1,-1,1,-1,-1,2,-1,-1,3,-1,-1,4,-1);
    const __m128i m_bg1 = _mm_setr_epi8(11,-1,12,13,-1,14,15,-1,-1,-1,-1,-1,-1,-1,-1,-1);
    const __m128i m_r1  = _mm_setr_epi8(-1,5,-1,-1,6,-1,-1,7,-1,-1,-1,-1,-1,-1,-1,-1);

    for (int y = 0; y < h; ++y) {
        const BYTE* yrow  = src    + (size_t)y * strideY;
        const BYTE* uvrow = uvbase + (size_t)(y >> 1) * strideUV;
        BYTE* drow = dst + (size_t)y * w * 3;
        int x = 0;
        for (; x + 8 <= w; x += 8) {
            __m128i Y = _mm_unpacklo_epi8(_mm_loadl_epi64((const __m128i*)(yrow + x)), zero);
            __m128i C = _mm_sub_epi16(Y, c16);

            // 8 px need 4 chroma samples; in a packed NV12 row the chroma for
            // pixel x starts at byte x.
            __m128i UV = _mm_unpacklo_epi8(_mm_loadl_epi64((const __m128i*)(uvrow + x)), zero);
            __m128i D = _mm_shufflehi_epi16(_mm_shufflelo_epi16(UV, _MM_SHUFFLE(2,2,0,0)),
                                            _MM_SHUFFLE(2,2,0,0));
            __m128i E = _mm_shufflehi_epi16(_mm_shufflelo_epi16(UV, _MM_SHUFFLE(3,3,1,1)),
                                            _MM_SHUFFLE(3,3,1,1));
            D = _mm_sub_epi16(D, c128);
            E = _mm_sub_epi16(E, c128);

            __m128i bg = _mm_unpacklo_epi8(Nv12Bgr8(C, D, E, 0),   // B
                                           Nv12Bgr8(C, D, E, 1));   // G
            __m128i R  = Nv12Bgr8(C, D, E, 2);
            __m128i c0 = _mm_or_si128(_mm_shuffle_epi8(bg, m_bg0),
                                      _mm_shuffle_epi8(R,  m_r0));
            __m128i c1 = _mm_or_si128(_mm_shuffle_epi8(bg, m_bg1),
                                      _mm_shuffle_epi8(R,  m_r1));
            BYTE* d = drow + (size_t)x * 3;
            _mm_storeu_si128((__m128i*)d, c0);          // bytes 0..15
            _mm_storel_epi64((__m128i*)(d + 16), c1);   // bytes 16..23
        }
        for (; x < w; ++x) {                            // tail
            const int Y = yrow[x];
            const int U = uvrow[(x >> 1) * 2];
            const int V = uvrow[(x >> 1) * 2 + 1];
            const int C = Y - 16, D = U - 128, E = V - 128;
            drow[x * 3 + 0] = Clip8((298 * C + 541 * D + 128) >> 8);
            drow[x * 3 + 1] = Clip8((298 * C - 55 * D - 136 * E + 128) >> 8);
            drow[x * 3 + 2] = Clip8((298 * C + 459 * E + 128) >> 8);
        }
    }
}
#endif  // x86

static void Nv12ToBgr24(const BYTE* src, int w, int h, int strideY, int strideUV,
                        BYTE* dst) {
#if defined(_M_X64) || defined(_M_IX86)
    if (g_convSSSE3) { Nv12ToBgr24_simd(src, w, h, strideY, strideUV, dst); return; }
#endif
    Nv12ToBgr24_scalar(src, w, h, strideY, strideUV, dst);
}

// Converts packed BGR24 back to NV12. Chroma is averaged over each 2x2 block,
// which is the correct inverse of the replication used above (picking one pixel
// instead would bias chroma toward whichever sample DLSSNR happened to move).
// Copies one frame (whatever the graph's pixel format is) into packed top-down
// BGR24, which is exactly what the panel needs for StretchDIBits.
static void DlssNrToBgr24(const BYTE* src, int w, int h, int bpp, bool isYuv,
                          int stride, BYTE* dst) {
    const int packed = w * 3;
    if (isYuv) { Nv12ToBgr24(src, w, h, stride, stride, dst); return; }
    if (bpp == 3) {
        if (stride == packed) {
            memcpy(dst, src, (size_t)packed * h);
        } else {
            for (int y = 0; y < h; ++y)
                memcpy(dst + (size_t)y * packed, src + (size_t)y * stride, packed);
        }
        return;
    }
    for (int y = 0; y < h; ++y) {                 // RGB32: drop the unused byte
        const BYTE* s = src + (size_t)y * stride;
        BYTE* d = dst + (size_t)y * packed;
        for (int x = 0; x < w; ++x) {
            d[x * 3 + 0] = s[x * 4 + 0];
            d[x * 3 + 1] = s[x * 4 + 1];
            d[x * 3 + 2] = s[x * 4 + 2];
        }
    }
}

// Mirrors the frame the filter just processed into the preview buffer: original on
// the left half of the buffer, engine output on the right. Called from the streaming
// path, so it only runs while the compare window is open.
static void DlssNrPreviewFrame(const BYTE* pOrig, const BYTE* pProc, int w, int h,
                               int bpp, bool isYuv, int stride) {
    if (!g_prevOn || w <= 0 || h <= 0 || !pOrig || !pProc) return;
    const size_t need = (size_t)w * 3 * h;
    EnterCriticalSection(&g_prevCs);
    if (g_prevCap < need) {
        BYTE* nb = (BYTE*)realloc(g_prevBuf, need * 2);
        if (!nb) { LeaveCriticalSection(&g_prevCs); return; }
        g_prevBuf = nb;
        g_prevCap = need;
    }
    DlssNrToBgr24(pOrig, w, h, bpp, isYuv, stride, g_prevBuf);
    DlssNrToBgr24(pProc, w, h, bpp, isYuv, stride, g_prevBuf + need);
    g_prevW = w;
    g_prevH = h;
    LeaveCriticalSection(&g_prevCs);
    DlssNrPreviewTouch();
}

static void Bgr24ToNv12(const BYTE* src, int w, int h, int strideY, int strideUV,
                        BYTE* dst) {
    for (int y = 0; y < h; ++y) {
        const BYTE* srow = src + (size_t)y * w * 3;
        BYTE* yrow = dst + (size_t)y * strideY;
        for (int x = 0; x < w; ++x) {
            const int B = srow[x * 3 + 0], G = srow[x * 3 + 1], R = srow[x * 3 + 2];
            const int yv = ((47 * R + 157 * G + 16 * B + 128) >> 8) + 16;
            yrow[x] = Clip8(yv);
        }
    }
    const int cw = (w + 1) / 2, ch = (h + 1) / 2;
    for (int cy = 0; cy < ch; ++cy) {
        BYTE* uv = dst + (size_t)strideY * h + (size_t)cy * strideUV;
        for (int cx = 0; cx < cw; ++cx) {
            int sr = 0, sg = 0, sb = 0, n = 0;
            for (int dy = 0; dy < 2; ++dy) {
                const int y = cy * 2 + dy;
                if (y >= h) break;
                const BYTE* srow = src + (size_t)y * w * 3;
                for (int dx = 0; dx < 2; ++dx) {
                    const int x = cx * 2 + dx;
                    if (x >= w) break;
                    sb += srow[x * 3 + 0]; sg += srow[x * 3 + 1]; sr += srow[x * 3 + 2];
                    ++n;
                }
            }
            if (!n) n = 1;
            const int R = sr / n, G = sg / n, B = sb / n;
            const int u = ((-26 * R - 87 * G + 112 * B + 128) >> 8) + 128;
            const int v = ((112 * R - 102 * G - 10 * B + 128) >> 8) + 128;
            uv[cx * 2 + 0] = Clip8(u);
            uv[cx * 2 + 1] = Clip8(v);
        }
    }
}



static void FreeMediaType(AM_MEDIA_TYPE& mt) {
    if (mt.cbFormat && mt.pbFormat) CoTaskMemFree(mt.pbFormat);
    if (mt.pUnk) mt.pUnk->Release();
    mt.cbFormat = 0; mt.pbFormat = nullptr; mt.pUnk = nullptr;
}

static HRESULT CopyMediaType(AM_MEDIA_TYPE& dst, const AM_MEDIA_TYPE* src) {
    if (!src) return E_POINTER;
    dst = *src;
    if (src->cbFormat && src->pbFormat) {
        dst.pbFormat = (BYTE*)CoTaskMemAlloc(src->cbFormat);
        if (!dst.pbFormat) { dst.cbFormat = 0; return E_OUTOFMEMORY; }
        memcpy(dst.pbFormat, src->pbFormat, src->cbFormat);
    } else {
        dst.pbFormat = nullptr; dst.cbFormat = 0;
    }
    if (dst.pUnk) dst.pUnk->AddRef();
    return S_OK;
}

static void MakeVideoType(AM_MEDIA_TYPE& mt, const AM_MEDIA_TYPE* ref, int w, int h, int bpp,
                          bool wantVideoInfo2 = false) {
    ZeroMemory(&mt, sizeof(mt));
    // No geometry => publish NOTHING. Inventing a size here (a previous version
    // hardcoded 1920x1080) is actively harmful: the graph builder takes the
    // enumerated type at face value and then connects with a geometry that does
    // not match the real stream, so the graph never completes.
    if (w <= 0 || h <= 0) return;
    mt.majortype  = MEDIATYPE_Video;
    if (bpp == 0) {
        // YUV 4:2:0 (NV12): the format players actually decode to.
        mt.subtype = MEDIASUBTYPE_NV12;
    } else {
        mt.subtype = (bpp == 4) ? MEDIASUBTYPE_RGB32 : MEDIASUBTYPE_RGB24;
    }
    mt.bFixedSizeSamples    = TRUE;
    mt.bTemporalCompression = FALSE;
    mt.lSampleSize = (bpp == 0) ? (ULONG)(StrideOf(w, 1) * h * 3 / 2)
                                : (ULONG)(StrideOf(w, bpp) * h);
    // Mirror the reference's format type when it uses VideoInfo2: decoders and
    // renderers that negotiated that variant expect it back, and a VideoInfo
    // reply can be refused outright.
    const bool useV2 = wantVideoInfo2 ||
                       (ref && ref->formattype == FORMAT_VideoInfo2 && IsVideoInfoFormat(ref));
    mt.formattype = useV2 ? FORMAT_VideoInfo2 : FORMAT_VideoInfo;
    mt.cbFormat   = useV2 ? sizeof(VIDEOINFOHEADER2) : sizeof(VIDEOINFOHEADER);
    mt.pbFormat   = (BYTE*)CoTaskMemAlloc(mt.cbFormat);
    if (!mt.pbFormat) { mt.cbFormat = 0; return; }
    ZeroMemory(mt.pbFormat, mt.cbFormat);
    BITMAPINFOHEADER* bmi = BitmapHeaderOf(&mt);
    if (!bmi) { CoTaskMemFree(mt.pbFormat); mt.pbFormat = nullptr; mt.cbFormat = 0; return; }
    // copy any reference fields first (timing, aspect, interlacing flags)
    if (ref && IsVideoInfoFormat(ref) && ref->formattype == mt.formattype &&
        ref->cbFormat >= mt.cbFormat) {
        memcpy(mt.pbFormat, ref->pbFormat, mt.cbFormat);
    }
    bmi->biSize     = sizeof(BITMAPINFOHEADER);
    bmi->biWidth    = w;
    bmi->biHeight   = h;
    bmi->biPlanes   = 1;
    if (bpp == 0) {
        // YUV types must carry the FOURCC so decoders/renderers recognise them.
        bmi->biBitCount    = 12;
        bmi->biCompression = MAKEFOURCC('N', 'V', '1', '2');
    } else {
        bmi->biBitCount    = (WORD)(bpp * 8);
        bmi->biCompression = BI_RGB;
    }
    bmi->biSizeImage = mt.lSampleSize;
}

// Human-readable subtype, for logging and the panel.
static const char* SubtypeName(const AM_MEDIA_TYPE* pmt) {
    if (!pmt) return "none";
    if (pmt->subtype == MEDIASUBTYPE_RGB24) return "RGB24";
    if (pmt->subtype == MEDIASUBTYPE_RGB32) return "RGB32";
    if (pmt->subtype == MEDIASUBTYPE_NV12)  return "NV12";
    if (pmt->subtype == MEDIASUBTYPE_YV12)  return "YV12";
    if (pmt->subtype == MEDIASUBTYPE_IYUV)  return "IYUV";
    if (pmt->subtype == MEDIASUBTYPE_I420_G) return "I420";
    return "other";
}

// Full description for the connection log: subtype + geometry + format type.
static std::string DescribeType(const AM_MEDIA_TYPE* pmt) {
    if (!pmt) return "none";
    char buf[160];
    const BITMAPINFOHEADER* bmi = BitmapHeaderOf(pmt);
    const char* fmt = (pmt->formattype == FORMAT_VideoInfo)  ? "VINFO"
                    : (pmt->formattype == FORMAT_VideoInfo2) ? "VINFO2" : "OTHERFMT";
    if (bmi)
        _snprintf_s(buf, sizeof(buf), _TRUNCATE, "%s %dx%d bpp=%d fourcc=0x%08X %s cb=%lu",
                    SubtypeName(pmt), bmi->biWidth, bmi->biHeight, bmi->biBitCount,
                    (unsigned)bmi->biCompression, fmt, (unsigned long)pmt->cbFormat);
    else
        _snprintf_s(buf, sizeof(buf), _TRUNCATE, "%s (no bmi) %s cb=%lu",
                    SubtypeName(pmt), fmt, (unsigned long)pmt->cbFormat);
    return std::string(buf);
}


// ---------------------------------------------------------------------------
// IEnumMediaTypes. Offers NV12 first (what players actually decode to), then the
// packed RGB formats -- each in BOTH VideoInfo and VideoInfo2 form, because
// graphs differ in which one they negotiate.
// bpp encoding: 0 = NV12, 3 = RGB24, 4 = RGB32.
// ---------------------------------------------------------------------------
struct MtSpec { int bpp; bool v2; };
static const MtSpec kPubTypes[] = {
    { 0, false }, { 0, true },
    { 3, false }, { 3, true },
    { 4, false }, { 4, true },
};
static const int kPubTypeCount = (int)(sizeof(kPubTypes) / sizeof(kPubTypes[0]));

class CEnumMediaTypes : public IEnumMediaTypes {
public:
    // ref may be NULL: the enumerator then publishes bare geometry, which is
    // what a decoder asks for BEFORE anything is connected.
    CEnumMediaTypes(const AM_MEDIA_TYPE* ref, int w, int h) : m_ref(1), m_pos(0), m_count(0) {
        for (int i = 0; i < kPubTypeCount; ++i) {
            ZeroMemory(&m_types[i], sizeof(AM_MEDIA_TYPE));
            MakeVideoType(m_types[i], ref, w, h, kPubTypes[i].bpp, kPubTypes[i].v2);
            if (m_types[i].pbFormat) ++m_count;
        }
    }
    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) {
        if (!ppv) return E_POINTER;
        if (riid == IID_IUnknown || riid == IID_IEnumMediaTypes) { *ppv = this; AddRef(); return S_OK; }
        *ppv = nullptr; return E_NOINTERFACE;
    }
    STDMETHODIMP_(ULONG) AddRef() { return InterlockedIncrement(&m_ref); }
    STDMETHODIMP_(ULONG) Release() {
        LONG r = InterlockedDecrement(&m_ref);
        if (r == 0) delete this;
        return r;
    }
    STDMETHODIMP Next(ULONG c, AM_MEDIA_TYPE** pp, ULONG* fetched) {
        if (!pp) return E_POINTER;
        ULONG n = 0;
        while (n < c && m_pos < (ULONG)m_count) {
            // pp[] is an OUTPUT array: the caller passes room for c POINTERS and
            // expects the callee to allocate each AM_MEDIA_TYPE. The slots arrive
            // UNINITIALISED, so dereferencing *pp[n] writes through garbage --
            // that was a real access violation in PotPlayer (dll+0x2FE7).
            AM_MEDIA_TYPE* p = (AM_MEDIA_TYPE*)CoTaskMemAlloc(sizeof(AM_MEDIA_TYPE));
            if (!p) break;
            if (FAILED(CopyMediaType(*p, &m_types[m_pos]))) { CoTaskMemFree(p); break; }
            pp[n] = p;
            ++n; ++m_pos;
        }
        if (fetched) *fetched = n;
        return (n == c) ? S_OK : S_FALSE;
    }
    STDMETHODIMP Skip(ULONG c) { m_pos += c; return (m_pos <= (ULONG)m_count) ? S_OK : S_FALSE; }
    STDMETHODIMP Reset() { m_pos = 0; return S_OK; }
    STDMETHODIMP Clone(IEnumMediaTypes** pp) {
        if (!pp) return E_POINTER;
        CEnumMediaTypes* p = new CEnumMediaTypes();
        if (!p) return E_OUTOFMEMORY;
        p->m_pos = m_pos; p->m_count = m_count;
        // Only copy the entries that are actually live, and zero the rest so the
        // destructor's FreeMediaType() can never touch an uninitialised slot.
        for (int i = 0; i < kPubTypeCount; ++i) {
            ZeroMemory(&p->m_types[i], sizeof(AM_MEDIA_TYPE));
            if (i < m_count) CopyMediaType(p->m_types[i], &m_types[i]);
        }
        *pp = p; return S_OK;
    }
private:
    CEnumMediaTypes() : m_ref(1), m_pos(0), m_count(0) {}
    ~CEnumMediaTypes() { for (int i = 0; i < kPubTypeCount; ++i) FreeMediaType(m_types[i]); }
    LONG m_ref; ULONG m_pos; int m_count;
    AM_MEDIA_TYPE m_types[kPubTypeCount];
};

// ---------------------------------------------------------------------------
// forward decls
// ---------------------------------------------------------------------------
class CDlssNrFilter;
class CInputPin;
class COutputPin;

static LONG g_lockCount = 0;   // DllCanUnloadNow() gate

// ---------------------------------------------------------------------------
// pin base: implements IPin + IUnknown once, shared by both directions
// ---------------------------------------------------------------------------
class CPinBase : public IPin {
public:
    CPinBase(CDlssNrFilter* f, LPCWSTR name, PIN_DIRECTION dir)
        : m_ref(1), m_pFilter(f), m_pConnected(nullptr), m_dir(dir), m_mtValid(false) {
        wcsncpy_s(m_name, _countof(m_name), name, _TRUNCATE);
        ZeroMemory(&m_mt, sizeof(m_mt));
    }
    virtual ~CPinBase() { DisconnectInternal(); }

    // ---- IUnknown (single implementation; IMemInputPin is a forwarding
    //      helper rather than a second base, which avoids the classic
    //      ambiguous-AddRef problem when inheriting two IUnknowns) ----
    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) {
        if (!ppv) return E_POINTER;
        if (riid == IID_IUnknown || riid == IID_IPin) {
            LogProbeOnce(m_name, riid, true);
            *ppv = static_cast<IPin*>(this); AddRef(); return S_OK;
        }
        LogProbeOnce(m_name, riid, false);
        *ppv = nullptr; return E_NOINTERFACE;
    }
    STDMETHODIMP_(ULONG) AddRef() { return InterlockedIncrement(&m_ref); }
    STDMETHODIMP_(ULONG) Release() {
        LONG r = InterlockedDecrement(&m_ref);
        if (r == 0) delete this;
        return r;
    }

    // ---- IPin ----
    STDMETHODIMP Connect(IPin* pReceivePin, const AM_MEDIA_TYPE* pmt) {
        if (!pReceivePin) return E_POINTER;
        if (m_pConnected) return VFW_E_ALREADY_CONNECTED;

        // Log every attempt: when a player silently declines to insert the
        // filter, these lines are the only way to see whether it tried at all.
        LogRaw("Connect(%ls) pmt=%s", m_name, pmt ? DescribeType(pmt).c_str() : "(null)");

        AM_MEDIA_TYPE mt;
        ZeroMemory(&mt, sizeof(mt));
        if (pmt) {
            if (!IsSupportedType(pmt)) {
                LogRaw("  -> REFUSED: unsupported type");
                return VFW_E_TYPE_NOT_ACCEPTED;
            }
            if (FAILED(CopyMediaType(mt, pmt))) return E_OUTOFMEMORY;
        } else {
            // no type offered: propose our first supported type using whatever
            // geometry the peer advertises
            AM_MEDIA_TYPE peer;
            ZeroMemory(&peer, sizeof(peer));
            int w = 0, h = 0;
            if (pReceivePin->ConnectionMediaType(&peer) == S_OK) {
                const BITMAPINFOHEADER* bmi = BitmapHeaderOf(&peer);
                if (bmi) { w = bmi->biWidth; h = bmi->biHeight; if (h < 0) h = -h; }
            }
            FreeMediaType(peer);
            LogRaw("  (no type offered; peer geometry %dx%d)", w, h);
            if (w <= 0 || h <= 0) return VFW_E_TYPE_NOT_ACCEPTED;
            MakeVideoType(mt, nullptr, w, h, 3);
            if (!mt.pbFormat) return E_OUTOFMEMORY;
        }

        HRESULT hr = pReceivePin->ReceiveConnection(static_cast<IPin*>(this), &mt);
        if (FAILED(hr)) { FreeMediaType(mt); return hr; }

        m_pConnected = pReceivePin;
        m_pConnected->AddRef();
        FreeMediaType(m_mt);
        CopyMediaType(m_mt, &mt);
        m_mtValid = true;
        FreeMediaType(mt);

        OnConnected();
        return S_OK;
    }

    STDMETHODIMP ReceiveConnection(IPin* pConnector, const AM_MEDIA_TYPE* pmt) {
        if (!pConnector || !pmt) return E_POINTER;
        LogRaw("ReceiveConnection(%ls) <- %s", m_name, DescribeType(pmt).c_str());
        if (m_pConnected) { LogRaw("  -> REFUSED: already connected"); return VFW_E_ALREADY_CONNECTED; }
        if (!IsSupportedType(pmt)) { LogRaw("  -> REFUSED: unsupported type"); return VFW_E_TYPE_NOT_ACCEPTED; }
        m_pConnected = pConnector;
        m_pConnected->AddRef();
        FreeMediaType(m_mt);
        if (FAILED(CopyMediaType(m_mt, pmt))) { m_mtValid = false; return E_OUTOFMEMORY; }
        m_mtValid = true;
        OnConnected();
        LogRaw("  -> ACCEPTED");
        return S_OK;
    }

    void DisconnectInternal() {
        if (m_pConnected) { m_pConnected->Release(); m_pConnected = nullptr; }
        FreeMediaType(m_mt);
        m_mtValid = false;
        OnDisconnected();
    }

    STDMETHODIMP Disconnect() { DisconnectInternal(); return S_OK; }

    STDMETHODIMP ConnectedTo(IPin** pp) {
        if (!pp) return E_POINTER;
        if (!m_pConnected) { *pp = nullptr; return VFW_E_NOT_CONNECTED; }
        *pp = m_pConnected; m_pConnected->AddRef(); return S_OK;
    }

    STDMETHODIMP ConnectionMediaType(AM_MEDIA_TYPE* pmt) {
        if (!pmt) return E_POINTER;
        if (!m_mtValid) { ZeroMemory(pmt, sizeof(*pmt)); return VFW_E_NOT_CONNECTED; }
        return CopyMediaType(*pmt, &m_mt);
    }

    STDMETHODIMP QueryPinInfo(PIN_INFO* pInfo) {
        if (!pInfo) return E_POINTER;
        // m_pFilter is a raw back-pointer: the graph can hold a pin reference
        // past the filter's lifetime, so never touch it unguarded.
        pInfo->pFilter = (IBaseFilter*)m_pFilter;
        if (pInfo->pFilter) pInfo->pFilter->AddRef();
        pInfo->dir = m_dir;
        wcsncpy_s(pInfo->achName, _countof(pInfo->achName), m_name, _TRUNCATE);
        return S_OK;
    }

    STDMETHODIMP QueryDirection(PIN_DIRECTION* p) { if (!p) return E_POINTER; *p = m_dir; return S_OK; }
    STDMETHODIMP QueryId(LPWSTR* Id) {
        if (!Id) return E_POINTER;
        size_t n = (wcslen(m_name) + 1) * sizeof(wchar_t);
        *Id = (LPWSTR)CoTaskMemAlloc(n);
        if (!*Id) return E_OUTOFMEMORY;
        memcpy(*Id, m_name, n);
        return S_OK;
    }
    STDMETHODIMP QueryAccept(const AM_MEDIA_TYPE* pmt) {
        // Logged because a graph builder may probe with this instead of trying a
        // connection; a silent S_FALSE here is a common reason a filter is never
        // inserted.
        const bool ok = IsSupportedType(pmt);
        LogRaw("QueryAccept(%ls) %s -> %s", m_name,
               pmt ? DescribeType(pmt).c_str() : "(null)", ok ? "S_OK" : "S_FALSE");
        return ok ? S_OK : S_FALSE;
    }
    STDMETHODIMP QueryInternalConnections(IPin** apPin, ULONG* nPin) {
        (void)apPin; if (nPin) *nPin = 0;
        return E_NOTIMPL;
    }
    STDMETHODIMP EndOfStream() { return OnEndOfStream(); }
    STDMETHODIMP BeginFlush()  { return OnBeginFlush(); }
    STDMETHODIMP EndFlush()    { return OnEndFlush(); }
    STDMETHODIMP NewSegment(REFERENCE_TIME tStart, REFERENCE_TIME tStop, double rate) {
        return OnNewSegment(tStart, tStop, rate);
    }

    // hooks
    virtual void    OnConnected() {}
    virtual void    OnDisconnected() {}
    virtual HRESULT OnEndOfStream() { return S_OK; }
    virtual HRESULT OnBeginFlush()  { return S_OK; }
    virtual HRESULT OnEndFlush()    { return S_OK; }
    virtual HRESULT OnNewSegment(REFERENCE_TIME, REFERENCE_TIME, double) { return S_OK; }

    // The negotiated type, or null if none. VIRTUAL because the output pin of a
    // transform filter must never hold a stale copy: its type is by definition
    // the input pin's type, so it derives it on demand (see COutputPin).
    virtual const AM_MEDIA_TYPE* MediaType() const { return m_mtValid ? &m_mt : nullptr; }
    IPin*   Connected()  const { return m_pConnected; }
    LPCWSTR Name()       const { return m_name; }

    // A transform filter's output type IS its input type. The output pin starts
    // out with no negotiated type of its own, so it adopts whatever the input
    // pin agreed to (called from the filter once the input connects).
    void AdoptType(const AM_MEDIA_TYPE* src) {
        FreeMediaType(m_mt);
        if (src && SUCCEEDED(CopyMediaType(m_mt, src))) m_mtValid = true;
        else m_mtValid = false;
    }

    // Called by the filter's destructor. Pins are refcounted and the graph may
    // outlive the filter, so drop the raw back-pointer to make any later call
    // fail cleanly instead of dereferencing freed memory.
    void Orphan() { m_pFilter = nullptr; }

protected:
    LONG          m_ref;
    CDlssNrFilter* m_pFilter;
    IPin*         m_pConnected;
    PIN_DIRECTION m_dir;
    AM_MEDIA_TYPE m_mt;
    bool          m_mtValid;
    wchar_t       m_name[128];
};

// ---------------------------------------------------------------------------
// input pin: IPin + a forwarding IMemInputPin
// ---------------------------------------------------------------------------
class CInputPin;

class CInputMemInputPin : public IMemInputPin {
public:
    CInputMemInputPin(CInputPin* owner) : m_ref(1), m_owner(owner) {}
    STDMETHODIMP QueryInterface(REFIID riid, void** ppv);
    STDMETHODIMP_(ULONG) AddRef() { return InterlockedIncrement(&m_ref); }
    STDMETHODIMP_(ULONG) Release() {
        LONG r = InterlockedDecrement(&m_ref);
        if (r == 0) delete this;
        return r;
    }
    STDMETHODIMP GetAllocator(IMemAllocator** pp);
    STDMETHODIMP NotifyAllocator(IMemAllocator* pAlloc, BOOL bReadOnly);
    STDMETHODIMP GetAllocatorRequirements(ALLOCATOR_PROPERTIES* p);
    STDMETHODIMP Receive(IMediaSample* pSample);
    STDMETHODIMP ReceiveMultiple(IMediaSample** pSamples, long n, long* nProcessed);
    STDMETHODIMP ReceiveCanBlock();
private:
    LONG m_ref;
    CInputPin* m_owner;
};

class CInputPin : public CPinBase {
public:
    CInputPin(CDlssNrFilter* f) : CPinBase(f, L"Input", PINDIR_INPUT), m_memIn(this) {}

    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) {
        if (!ppv) return E_POINTER;
        if (riid == IID_IMemInputPin) {
            LogProbeOnce("Input", riid, true);
            *ppv = &m_memIn; m_memIn.AddRef(); return S_OK;
        }
        return CPinBase::QueryInterface(riid, ppv);
    }

    STDMETHODIMP EnumMediaTypes(IEnumMediaTypes** pp) {
        if (!pp) return E_POINTER;
        // Enumerating must SUCCEED even before anything is connected (a decoder
        // asks first; returning VFW_E_NOT_CONNECTED made PotPlayer give up).
        // But it must not fabricate a geometry either -- an empty list is the
        // honest answer, and the builder will ask again after the upstream side
        // has settled on a real size.
        int w = 0, h = 0;
        Geometry(&w, &h);
        LogRaw("EnumMediaTypes(%ls) geometry=%dx%d -> %d types",
               m_name, w, h, (w > 0 && h > 0) ? kPubTypeCount : 0);
        *pp = new CEnumMediaTypes(MediaType(), w, h);
        return *pp ? S_OK : E_OUTOFMEMORY;
    }

    // Receive() is where the engine is driven; implemented on the filter.
    HRESULT DoReceive(IMediaSample* pSample);

    void Geometry(int* w, int* h) {
        const AM_MEDIA_TYPE* mt = MediaType();
        const BITMAPINFOHEADER* bmi = mt ? BitmapHeaderOf(mt) : nullptr;
        if (bmi) {
            *w = bmi->biWidth;
            *h = bmi->biHeight;
            if (*h < 0) *h = -*h;                 // negative = top-down
        } else { *w = 0; *h = 0; }
    }

    IMemAllocator* Allocator() { return m_pAlloc; }
    void SetAllocator(IMemAllocator* a) {
        if (m_pAlloc) m_pAlloc->Release();
        m_pAlloc = a;
        if (m_pAlloc) m_pAlloc->AddRef();
    }

    // overrides (defined after CDlssNrFilter, hence re-declared here)
    void    OnConnected() override;
    void    OnDisconnected() override;
    HRESULT OnBeginFlush() override;
    HRESULT OnEndFlush() override;
    HRESULT OnNewSegment(REFERENCE_TIME, REFERENCE_TIME, double) override;

private:
    CInputMemInputPin m_memIn;
    IMemAllocator*    m_pAlloc = nullptr;
};

STDMETHODIMP CInputMemInputPin::QueryInterface(REFIID riid, void** ppv) {
    if (!ppv) return E_POINTER;
    if (riid == IID_IUnknown || riid == IID_IMemInputPin) { *ppv = this; AddRef(); return S_OK; }
    *ppv = nullptr; return E_NOINTERFACE;
}
STDMETHODIMP CInputMemInputPin::GetAllocator(IMemAllocator** pp) {
    if (!pp) return E_POINTER;
    *pp = nullptr;
    return VFW_E_NOT_CONNECTED;      // upstream provides the allocator
}
STDMETHODIMP CInputMemInputPin::NotifyAllocator(IMemAllocator* pAlloc, BOOL) {
    m_owner->SetAllocator(pAlloc);
    return S_OK;
}
STDMETHODIMP CInputMemInputPin::GetAllocatorRequirements(ALLOCATOR_PROPERTIES* p) {
    (void)p; return E_NOTIMPL;
}
STDMETHODIMP CInputMemInputPin::Receive(IMediaSample* pSample) {
    return m_owner->DoReceive(pSample);
}
STDMETHODIMP CInputMemInputPin::ReceiveMultiple(IMediaSample** pSamples, long n, long* nProcessed) {
    if (!pSamples || !nProcessed) return E_POINTER;
    HRESULT hr = S_OK;
    long i = 0;
    for (; i < n; ++i) {
        hr = m_owner->DoReceive(pSamples[i]);
        if (FAILED(hr)) break;
    }
    *nProcessed = i;
    return hr;
}
STDMETHODIMP CInputMemInputPin::ReceiveCanBlock() { return S_OK; }   // we do block

// ---------------------------------------------------------------------------
// output pin
// ---------------------------------------------------------------------------
class COutputPin : public CPinBase {
public:
    COutputPin(CDlssNrFilter* f) : CPinBase(f, L"Output", PINDIR_OUTPUT) {}

    // A transform filter's output type IS its input type, so derive it on every
    // query instead of caching a copy.
    //
    // Caching was the cause of a long-standing failure in BOTH PotPlayer and
    // MPC-BE. A player probes the graph by repeatedly disconnecting and
    // reconnecting the input pin; each teardown went through
    // CPinBase::DisconnectInternal(), which invalidates the pin's stored media
    // type. A player that then asked the OUTPUT pin what it offered got "nothing"
    // and removed the filter from the graph without ever calling Connect() on
    // it. The field log showed exactly that transition:
    //     EnumMediaTypes(Output) geometry=1920x1440 hasType=1 -> 6 types
    //     ... player disconnects/reconnects the input ...
    //     EnumMediaTypes(Output) geometry=0x0     hasType=0 -> 0 types
    //     <filter removed>
    // Defined out of line below (CDlssNrFilter is incomplete here).
    const AM_MEDIA_TYPE* MediaType() const override;

    // Also overridden for the same reason: the base implementation reports
    // VFW_E_NOT_CONNECTED whenever the pin's own cached copy is invalid, which
    // is exactly the state a player's probe leaves it in.
    STDMETHODIMP ConnectionMediaType(AM_MEDIA_TYPE* pmt) override {
        if (!pmt) return E_POINTER;
        const AM_MEDIA_TYPE* mt = MediaType();
        if (!mt) { ZeroMemory(pmt, sizeof(*pmt)); return VFW_E_NOT_CONNECTED; }
        return CopyMediaType(*pmt, mt);
    }

    STDMETHODIMP EnumMediaTypes(IEnumMediaTypes** pp) {
        if (!pp) return E_POINTER;
        // Same rule as the input pin: succeed without a prior connection, and do
        // NOT invent geometry. Before the input connects the honest answer is an
        // empty list.
        int w = 0, h = 0;
        Geometry(&w, &h);
        const AM_MEDIA_TYPE* mt = MediaType();
        // Logged on the OUTPUT too: if this reports 0 types after the input is
        // connected, a graph builder has nothing to match and gives up WITHOUT
        // ever calling Connect() -- which is exactly the observed symptom.
        LogRaw("EnumMediaTypes(%ls) geometry=%dx%d hasType=%d -> %d types",
               m_name, w, h, mt ? 1 : 0, (w > 0 && h > 0) ? kPubTypeCount : 0);
        *pp = new CEnumMediaTypes(mt, w, h);
        return *pp ? S_OK : E_OUTOFMEMORY;
    }

    // Only offer exactly what the input agreed to: geometry and pixel format
    // must match, otherwise the engine's output would be the wrong shape.
    // NOTE: a NULL pmt means "you pick", which is a normal DirectShow call --
    // rejecting it breaks every downstream connection.
    STDMETHODIMP Connect(IPin* pReceivePin, const AM_MEDIA_TYPE* pmt) {
        // EVERY exit path is logged. The earlier version returned before any
        // logging, so a rejection here was invisible and the filter looked like
        // it simply vanished from the graph.
        const AM_MEDIA_TYPE* mine = MediaType();
        LogRaw("OUTPUT Connect: mine=%s offered=%s",
               mine ? DescribeType(mine).c_str() : "(none)",
               pmt ? DescribeType(pmt).c_str() : "(null)");
        if (!mine) {
            LogRaw("  -> REFUSED: output has no type (input not connected yet?)");
            return VFW_E_NOT_CONNECTED;
        }
        if (pmt && !SameShape(pmt, mine)) {
            LogRaw("  -> REFUSED: shape mismatch");
            return VFW_E_TYPE_NOT_ACCEPTED;
        }
        HRESULT hr = CPinBase::Connect(pReceivePin, mine);
        LogRaw("  CPinBase::Connect -> 0x%08X", (unsigned)hr);
        if (SUCCEEDED(hr)) {
            hr = DecideAllocator(pReceivePin);
            LogRaw("  DecideAllocator -> 0x%08X", (unsigned)hr);
        }
        return hr;
    }

    STDMETHODIMP QueryAccept(const AM_MEDIA_TYPE* pmt) {
        const AM_MEDIA_TYPE* mine = MediaType();
        if (!mine) return S_FALSE;
        return SameShape(pmt, mine) ? S_OK : S_FALSE;
    }

    STDMETHODIMP EndOfStream() {
        // Forward downstream. (Do NOT fabricate a null sample into the sink's
        // Receive: Receive(nullptr) is a crash, not an end-of-stream signal.)
        return m_pConnected ? m_pConnected->EndOfStream() : S_OK;
    }
    STDMETHODIMP BeginFlush() { return m_pConnected ? m_pConnected->BeginFlush() : S_OK; }
    STDMETHODIMP EndFlush()   { return m_pConnected ? m_pConnected->EndFlush()   : S_OK; }
    // A transform filter has to pass the segment through; before this the
    // renderer never saw one at all.
    STDMETHODIMP NewSegment(REFERENCE_TIME tStart, REFERENCE_TIME tStop, double rate) {
        return m_pConnected ? m_pConnected->NewSegment(tStart, tStop, rate) : S_OK;
    }

    IMemAllocator* Allocator() { return m_pAlloc; }

    void Geometry(int* w, int* h) {
        const AM_MEDIA_TYPE* mt = MediaType();
        const BITMAPINFOHEADER* bmi = mt ? BitmapHeaderOf(mt) : nullptr;
        if (bmi) {
            *w = bmi->biWidth;
            *h = bmi->biHeight;
            if (*h < 0) *h = -*h;
        } else { *w = 0; *h = 0; }
    }

private:
    static bool SameShape(const AM_MEDIA_TYPE* a, const AM_MEDIA_TYPE* b) {
        if (!IsSupportedType(a)) return false;
        if (a->subtype != b->subtype) return false;
        const BITMAPINFOHEADER* ba = BitmapHeaderOf(a);
        const BITMAPINFOHEADER* bb = BitmapHeaderOf(b);
        if (!ba || !bb) return false;
        return ba->biWidth  == bb->biWidth &&
               ba->biHeight == bb->biHeight;
    }

    // Use the downstream renderer's allocator when it offers one, otherwise
    // create the stock one. Either way the samples are ours to fill.
    HRESULT DecideAllocator(IPin* pDownstream) {
        IMemInputPin* pIn = nullptr;
        HRESULT hr = pDownstream->QueryInterface(IID_IMemInputPin, (void**)&pIn);
        if (FAILED(hr)) { LogRaw("  Alloc: no IMemInputPin 0x%08X", (unsigned)hr); return hr; }

        const AM_MEDIA_TYPE* mt = MediaType();
        int w = 0, h = 0;
        Geometry(&w, &h);
        if (w <= 0 || h <= 0) {
            // Never invent geometry here: this runs AFTER a type was negotiated,
            // so a missing size means the connection state is broken. Failing is
            // far better than allocating the wrong buffer size.
            LogRaw("DecideAllocator: no negotiated geometry, refusing");
            pIn->Release();
            return VFW_E_INVALIDMEDIATYPE;
        }
        const long need = SampleSizeOf(mt, w, h);

        // Allocator policy, which is what the output connection hinges on.
        //
        // DirectShow's contract for a transform filter: the UPSTREAM side
        // supplies the allocator, and it tells the downstream pin about it via
        // NotifyAllocator(). The renderer's own allocator is only meant to be
        // used when the downstream ping explicitly offers one and the upstream
        // agrees -- and EVR's allocator reports cbBuffer = 0 before it is
        // committed, then rejects SetProperties() with E_FAIL (0x80004005).
        // That rejection is exactly what broke the connection.
        //
        // So: always create and own our allocator. That is the documented
        // behaviour for a filter that produces its own output, and it avoids
        // fighting the renderer over a layout it has not committed yet.
        IMemAllocator* pAlloc = nullptr;
        hr = CoCreateInstance(CLSID_MemoryAllocator, nullptr, CLSCTX_INPROC_SERVER,
                              IID_IMemAllocator, (void**)&pAlloc);
        LogRaw("  Alloc: created own MemoryAllocator 0x%08X", (unsigned)hr);
        if (FAILED(hr)) { pIn->Release(); return hr; }

        ALLOCATOR_PROPERTIES req, act;
        ZeroMemory(&req, sizeof(req));
        req.cBuffers = 3;
        req.cbBuffer = need;
        req.cbAlign  = 1;
        req.cbPrefix = 0;

        hr = pAlloc->SetProperties(&req, &act);
        LogRaw("  Alloc: SetProperties(%ld bufs, %ld bytes, align %ld) -> 0x%08X",
               req.cBuffers, req.cbBuffer, req.cbAlign, (unsigned)hr);
        if (SUCCEEDED(hr)) hr = pAlloc->Commit();
        LogRaw("  Alloc: Commit -> 0x%08X", (unsigned)hr);

        if (SUCCEEDED(hr)) {
            pIn->NotifyAllocator(pAlloc, FALSE);
            if (m_pAlloc) m_pAlloc->Release();
            m_pAlloc = pAlloc;                 // keep our reference
        } else if (pAlloc) {
            pAlloc->Release();
        }
        pIn->Release();
        return hr;
    }

    IMemAllocator* m_pAlloc = nullptr;
};

// ---------------------------------------------------------------------------
// the filter
// ---------------------------------------------------------------------------
class CDlssNrFilter : public IBaseFilter, public ISpecifyPropertyPages {
public:
    CDlssNrFilter() : m_ref(1), m_state(State_Stopped), m_pGraph(nullptr), m_pClock(nullptr),
                      m_engine(Engine::Instance()), m_needReset(true),
                      m_scratchIn(nullptr), m_scratchOut(nullptr),
                      m_scratchCap(0), m_lastType{}, m_hasLastType(false) {
        // A new instance means another file/graph: allow a fresh init attempt
        // even if the previous file's engine gave up.
        m_engine.NewSession();
        InitializeCriticalSection(&m_streamCs);
        m_streamCsInit = true;
        // The panel pushes a refresh here the moment a parameter is applied, so
        // there is no polling at all: a paused graph has no frames and an edit
        // made while paused is the only thing that needs to repaint the picture.
        DlssNrAddRefreshSink(&CDlssNrFilter::RefreshThunk, this);
        m_in  = new CInputPin(this);
        m_out = new COutputPin(this);
        m_cfg = ReadConfig();
        // The engine is process-wide, so it is seeded from the ini exactly once
        // and afterwards its live options are authoritative -- that is what lets
        // the control panel toggle the master switch mid-playback.
        m_engine.SeedOptionsOnce(m_cfg.enabled, m_cfg.style, m_cfg.intensity,
                                 m_cfg.localTone, m_cfg.localStruct,
                                 m_cfg.skinStruct, m_cfg.autoMask);
        if (g_shared) m_lastSeq = g_shared->reqSeq;
        LogRaw("filter: created; config enabled=%d style=%d intensity=%d%% "
               "skinstructure=%d%% automask=%d",
               m_cfg.enabled ? 1 : 0, m_cfg.style, (int)(m_cfg.intensity * 100),
               (int)(m_cfg.skinStruct * 100), m_cfg.autoMask ? 1 : 0);
    }
    virtual ~CDlssNrFilter() {
        // Unregister first: the panel thread walks this list, so it must not see
        // an object that is being torn down.
        m_shutdown = true;
        DlssNrRemoveRefreshSink(&CDlssNrFilter::RefreshThunk, this);
        if (m_streamCsInit) { DeleteCriticalSection(&m_streamCs); m_streamCsInit = false; }
        // Orphan the pins FIRST: they are refcounted and may outlive us (the
        // graph can still call into them during teardown), so their raw
        // back-pointer must not dangle.
        if (m_in)  { m_in->Orphan();  m_in->DisconnectInternal();  m_in->Release(); }
        if (m_out) { m_out->Orphan(); m_out->DisconnectInternal(); m_out->Release(); }
        if (m_pGraph) m_pGraph->Release();
        if (m_pClock) m_pClock->Release();
        FreeMediaType(m_lastType);
        free(m_scratchIn);
        free(m_scratchOut);
        // Balance the reference taken in JoinFilterGraph; the graph does not
        // always send JoinFilterGraph(null) before dropping the filter.
        if (m_trayHooked) { m_trayHooked = false; DlssNrTray::Instance().FilterRemoved(); }
    }

    CInputPin*  Input()  { return m_in; }
    COutputPin* Output() { return m_out; }

    // ---- live control channel -------------------------------------------
    // Split in two so a panel change takes effect on the very frame it is
    // picked up: ApplyRequests() runs BEFORE processing, PublishTelemetry()
    // after. (Doing both at the end costs one frame of latency, which shows up
    // as a stale first frame right after moving a slider.)
    void ApplyRequests() {
        if (!g_shared) return;
        SharedState* s = g_shared;
        long seq = s->reqSeq;
        if (seq == m_lastSeq) return;
        m_lastSeq = seq;
        m_cfg.enabled     = s->reqEnabled != 0;
        m_cfg.style       = s->reqStyle;
        m_cfg.intensity   = Clamp01(s->reqIntensity);
        m_cfg.localTone   = Clamp01(s->reqLocalTone);
        m_cfg.localStruct = Clamp01(s->reqLocalStruct);
        // skinStruct / autoMask have no legacy request field: the request path
        // predates them and the in-process panel drives Engine::SetOptions()
        // directly, so keep the ini/current values here.
        // Push to the engine now (no-op if it is not ready; DoInit() re-applies
        // the cached values when the model finishes loading).
        m_engine.SetOptions(m_cfg.enabled, m_cfg.style, m_cfg.intensity,
                            m_cfg.localTone, m_cfg.localStruct,
                            m_cfg.skinStruct, m_cfg.autoMask);
        LogRaw("panel: enabled=%d style=%d intensity=%d%% tone=%d%% struct=%d%% "
               "skin=%d%% automask=%d",
               m_cfg.enabled ? 1 : 0, m_cfg.style,
               (int)(m_cfg.intensity * 100), (int)(m_cfg.localTone * 100),
               (int)(m_cfg.localStruct * 100), (int)(m_cfg.skinStruct * 100),
               m_cfg.autoMask ? 1 : 0);
        s->reqApplied = seq;
    }

    void PublishTelemetry(int w, int h, int bpp, bool processed, int reasonOverride) {
        if (!g_shared) return;
        SharedState* s = g_shared;
        InterlockedIncrement(&s->heartbeat);
        InterlockedIncrement(&s->framesSeen);
        if (processed) InterlockedIncrement(&s->framesProcessed);
        else           InterlockedIncrement(&s->framesPassthrough);
        s->engineReady   = m_engine.Ready() ? 1 : 0;
        s->engineGaveUp  = m_engine.GaveUp() ? 1 : 0;
        s->engineW       = m_engine.Width();
        s->engineH       = m_engine.Height();
        s->videoW        = w;
        s->videoH        = h;
        s->inputBpp      = bpp;
        s->enabled       = m_engine.OptEnabled() ? 1 : 0;
        s->lastProcessMs = m_engine.LastProcessMs();
        s->pid           = (long)GetCurrentProcessId();
        SetStatus(reasonOverride, "%s", m_statusText);
    }

    // ---- IUnknown / IPersist / IMediaFilter / IBaseFilter ----
    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) {
        if (!ppv) return E_POINTER;
        if (riid == IID_IUnknown || riid == IID_IBaseFilter) {
            LogProbeOnce("Filter", riid, true);
            *ppv = static_cast<IBaseFilter*>(this); AddRef(); return S_OK;
        }
        if (riid == IID_IMediaFilter) { LogProbeOnce("Filter", riid, true); *ppv = static_cast<IMediaFilter*>(this); AddRef(); return S_OK; }
        if (riid == IID_IPersist)     { LogProbeOnce("Filter", riid, true); *ppv = static_cast<IPersist*>(this);    AddRef(); return S_OK; }
        if (riid == IID_ISpecifyPropertyPages) {
            LogProbeOnce("Filter", riid, true);
            *ppv = static_cast<ISpecifyPropertyPages*>(this); AddRef(); return S_OK;
        }
        LogProbeOnce("Filter", riid, false);
        *ppv = nullptr; return E_NOINTERFACE;
    }
    STDMETHODIMP_(ULONG) AddRef() { return InterlockedIncrement(&m_ref); }
    STDMETHODIMP_(ULONG) Release() {
        LONG r = InterlockedDecrement(&m_ref);
        if (r == 0) delete this;
        return r;
    }

    STDMETHODIMP GetClassID(CLSID* pCls) {
        if (!pCls) return E_POINTER;
        *pCls = CLSID_DlssNrFilter;
        return S_OK;
    }

    // ---- ISpecifyPropertyPages ----
    // Hands the player's property frame the CLSID of our settings page, which
    // is what puts "DLSSNR" into Filters -> Properties (same mechanism LAV uses).
    STDMETHODIMP GetPages(CAUUID* pPages) {
        if (!pPages) return E_POINTER;
        pPages->pElems = (GUID*)CoTaskMemAlloc(sizeof(GUID));
        if (!pPages->pElems) { pPages->cElems = 0; return E_OUTOFMEMORY; }
        pPages->cElems = 1;
        pPages->pElems[0] = CLSID_DlssNrPage;
        return S_OK;
    }

    STDMETHODIMP Stop() {
        m_state = State_Stopped;
        // A seek/stop makes every queued sample worthless, and the renderer must
        // stop expecting one. Without the flush markers the graph could wait for
        // a delivery that will never be useful -- the seek hang.
        m_flushing = true;
        m_needReset = true;
        LogRaw("filter: Stop -> stopped (frames while paused: %d)", m_framesWhilePaused);
        return S_OK;
    }
    STDMETHODIMP Pause() {
        m_state = State_Paused;
        m_flushing = false;                 // EndFlush may never arrive
        // Now that a format is locked in, warm the engine up. Async: the graph
        // keeps running and frames pass through until the model is loaded.
        int w = 0, h = 0;
        if (m_in) m_in->Geometry(&w, &h);
        LogRaw("filter: Pause (input %dx%d, in-conn=%d out-conn=%d)",
               w, h, m_in && m_in->Connected() ? 1 : 0, m_out && m_out->Connected() ? 1 : 0);
        // Warm the engine up even when disabled: the master switch is live now,
        // and it can only be instant if the model is already loaded. Skip the
        // request when the session already matches -- a seek pings Pause a lot
        // and there is no reason to touch the engine on each one.
        if (w > 0 && h > 0 && !(m_engine.Ready() &&
                                m_engine.Width() == w && m_engine.Height() == h)) {
            LogRaw("filter: Pause -> requesting engine init %dx%d", w, h);
            m_engine.RequestInit(w, h);
        }
        return S_OK;
    }
    STDMETHODIMP Run(REFERENCE_TIME) {
        m_state = State_Running;
        m_flushing = false;
        m_needReset = true;
        LogRaw("filter: Run -> running (paused-time frames seen: %d)", m_framesWhilePaused);
        m_framesWhilePaused = 0;
        m_droppedFlush = m_droppedStopped = 0;
        return S_OK;
    }
    STDMETHODIMP GetState(DWORD, FILTER_STATE* pState) {
        if (!pState) return E_POINTER;
        *pState = m_state;
        return S_OK;
    }
    STDMETHODIMP SetSyncSource(IReferenceClock* pClock) {
        if (m_pClock) m_pClock->Release();
        m_pClock = pClock;
        if (m_pClock) m_pClock->AddRef();
        return S_OK;
    }
    STDMETHODIMP GetSyncSource(IReferenceClock** ppClock) {
        if (!ppClock) return E_POINTER;
        *ppClock = m_pClock;
        if (m_pClock) m_pClock->AddRef();
        return S_OK;
    }

    STDMETHODIMP EnumPins(IEnumPins** ppEnum);
    STDMETHODIMP FindPin(LPCWSTR Id, IPin** ppPin) {
        if (!Id || !ppPin) return E_POINTER;
        *ppPin = nullptr;
        if (_wcsicmp(Id, L"Input") == 0)  { *ppPin = m_in;  m_in->AddRef();  return S_OK; }
        if (_wcsicmp(Id, L"Output") == 0) { *ppPin = m_out; m_out->AddRef(); return S_OK; }
        return VFW_E_NOT_FOUND;
    }
    STDMETHODIMP QueryFilterInfo(FILTER_INFO* pInfo) {
        if (!pInfo) return E_POINTER;
        wcsncpy_s(pInfo->achName, _countof(pInfo->achName), kFilterName, _TRUNCATE);
        pInfo->pGraph = m_pGraph;
        if (m_pGraph) m_pGraph->AddRef();
        return S_OK;
    }
    STDMETHODIMP JoinFilterGraph(IFilterGraph* pGraph, LPCWSTR) {
        // Logged because "is the filter actually IN a graph?" is the single most
        // useful fact when it is created but never receives frames.
        LogRaw("JoinFilterGraph(%p) %s", (void*)pGraph, pGraph ? "ADDED to a graph" : "REMOVED");
        // The tray icon exists exactly while a DLSSNR filter is in a graph, so
        // it appears with playback and disappears when the player lets go.
        if (pGraph && !m_trayHooked)      { m_trayHooked = true;  DlssNrTray::Instance().FilterAdded(); }
        else if (!pGraph && m_trayHooked) { m_trayHooked = false; DlssNrTray::Instance().FilterRemoved(); }
        if (m_pGraph) m_pGraph->Release();
        m_pGraph = pGraph;
        if (m_pGraph) m_pGraph->AddRef();
        return S_OK;
    }
    STDMETHODIMP QueryVendorInfo(LPWSTR* pVendor) {
        if (!pVendor) return E_POINTER;
        const wchar_t* v = L"DLSS5Tool";
        size_t n = (wcslen(v) + 1) * sizeof(wchar_t);
        *pVendor = (LPWSTR)CoTaskMemAlloc(n);
        if (!*pVendor) return E_OUTOFMEMORY;
        memcpy(*pVendor, v, n);
        return S_OK;
    }

    // ---- the paused refresh: SHELVED ---------------------------------------
    //
    // A paused graph delivers no samples, so an edit made while paused has nothing to
    // affect -- and the frozen frame belongs to the player, not to this filter. Every
    // mechanism a filter can reach was tried and measured; see RefreshIfNeeded() for
    // the list. None of them make a paused picture change, so nothing is attempted any
    // more: a paused edit only updates the engine options, and the next frame that
    // plays carries them.

    // Called by the panel/tray (any thread) right after a parameter changed.
    static void RefreshThunk(void* ctx, bool committed) {
        ((CDlssNrFilter*)ctx)->RequestRefresh(committed);
    }

    void RequestRefresh(bool) {
        // The engine re-run costs 10-30 ms. Doing it on the caller's thread (which is
        // the PANEL's own UI thread) is what made the sliders impossible to drag: the
        // turn starts on a pool thread instead, and a second request while one is
        // queued is simply dropped -- RefreshIfNeeded loops until the parameter
        // revision it rendered matches the latest one.
        if (m_shutdown) return;
        if (m_rebuildQueued) return;
        m_rebuildQueued = true;
        if (!TrySubmitThreadpoolCallback(&CDlssNrFilter::PoolRebuild, this, nullptr)) {
            m_rebuildQueued = false;
            RefreshIfNeeded();
        }
    }

    static VOID CALLBACK PoolRebuild(PTP_CALLBACK_INSTANCE, PVOID ctx) {
        CDlssNrFilter* self = (CDlssNrFilter*)ctx;
        self->m_rebuildQueued = false;
        self->RefreshIfNeeded();
    }

    void RefreshIfNeeded() {
        if (m_shutdown) return;
        // The panel's 对比 view is the one place where a parameter change made while
        // paused CAN be seen: the engine re-runs the frame the preview kept and the
        // panel repaints from that buffer. Nothing here touches the player's graph.
        // While playback is running the streaming path mirrors every frame anyway, so
        // there is nothing to re-run (and doing it would just fight the live frames).
        if (DlssNrPreviewWant() && m_state == State_Paused) {
            // Loop until we have rendered the LATEST parameter revision: a slider drag
            // bumps it faster than one engine run takes, and rendering a stale set and
            // stopping is what made the right half flicker between two images.
            for (int guard = 0; guard < 4; ++guard) {
                const LONG rev = m_engine.ParameterRevision();
                RebuildPreview();
                if (m_engine.ParameterRevision() == rev) break;
            }
            return;
        }
        // SHELVED on the player's side: a parameter change made while the picture is
        // frozen cannot be made visible there. Every path a filter can reach was
        // measured:
        //   * pushing a re-rendered sample -> the renderer queues it, never paints it,
        //     and the queue flashes out on unpause;
        //   * IMediaSeeking::SetPositions -> a paused parser returns the previous
        //     keyframe, so the picture AND the position jump backwards;
        //   * IMediaControl::Run() + immediate Pause() -> nothing changes at all;
        //   * IVideoFrameStep::Step(1) -> S_OK and a frame really was delivered, but
        //     PotPlayer does not repaint a paused picture even for its OWN "next
        //     frame" command. That is what closed the case: the player owns the
        //     frozen frame, so no filter-side mechanism can win.
        // A paused edit therefore just updates the engine options; the next frame that
        // plays carries the new parameters.
        if (m_state == State_Paused && !m_loggedPausedEdit) {
            m_loggedPausedEdit = true;
            LogRaw("refresh: paused edits cannot repaint the player's picture (shelved) - "
                   "use the panel's compare view, or resume playback");
        }
    }

    // Re-renders the frame kept in the preview buffer with the current parameters and
    // hands it back to the panel. This is what makes a paused edit visible at all.
    void RebuildPreview() {
        size_t need = 0;
        // m_scratchIn/Out are shared with the streaming path, so the whole copy ->
        // engine -> write-back runs under the stream lock. Without this the two paths
        // trampled each other's buffers and the picture flickered.
        EnterCriticalSection(&m_streamCs);
        const int w = g_prevW;
        const int h = g_prevH;
        need = (size_t)(w > 0 ? w : 0) * 3 * (size_t)(h > 0 ? h : 0);
        if (g_prevBuf && w > 0 && h > 0 && g_prevCap >= need) {
            const size_t nv12Need = (size_t)w * h * 3 / 2;
            bool refreshed = false;
            // EnsureScratch MUST run before anything touches m_scratchIn/Out.
            // It is the only place they are allocated, and the streaming path no
            // longer calls it (the engine takes NV12 straight through), so there
            // is no longer any earlier path that guarantees they exist. Writing
            // the memcpy before this call crashed with 0xC0000005 on the first
            // parameter edit made with the compare window open.
            if (EnsureScratch(need)) {
                EnterCriticalSection(&g_prevCs);
                memcpy(m_scratchIn, g_prevBuf, need);
                LeaveCriticalSection(&g_prevCs);

                if (g_prevNv12Cap < nv12Need) {
                    BYTE* nb = (BYTE*)realloc(g_prevNv12, nv12Need * 2);
                    if (nb) { g_prevNv12 = nb; g_prevNv12Cap = nv12Need; }
                }
                if (g_prevNv12 && g_prevNv12Cap >= nv12Need) {
                    BYTE* nvIn  = g_prevNv12;
                    BYTE* nvOut = g_prevNv12 + nv12Need;
                    Bgr24ToNv12(m_scratchIn, w, h, w, w, nvIn);
                    if (m_engine.ProcessNv12(nvIn, w, nvIn + (size_t)w * h, w,
                                             nvOut, w, nvOut + (size_t)w * h, w,
                                             w, h, false)) {
                        DlssNrToBgr24(nvOut, w, h, 3, true, w, m_scratchOut);
                        refreshed = true;
                    }
                }
            }
            if (refreshed) {
                EnterCriticalSection(&g_prevCs);
                if (g_prevBuf && g_prevCap >= need) {
                    memcpy(g_prevBuf + need, m_scratchOut, need);
                    DlssNrPreviewTouch();
                }
                LeaveCriticalSection(&g_prevCs);
            }
        }
        LeaveCriticalSection(&m_streamCs);
    }

    // Runs the engine (or the pure pass-through copy) for one frame. Shared by
    // the streaming path and the paused-refresh path.
    bool ProcessFrame(const BYTE* pSrc, BYTE* pDst, const AM_MEDIA_TYPE* mt,
                      int w, int h, int bpp, bool isYuv, int* reasonOut) {
        *reasonOut = kReasonNone;
        const int inStride  = StrideOf(w, isYuv ? 1 : bpp);
        const int outStride = inStride;
        const int packed = w * 3;
        const size_t need = (size_t)SampleSizeOf(mt, w, h);
        bool done = false;
        int reason = kReasonNone;
        // Master switch lives in the process-wide engine so the panel can flip
        // it live; m_cfg.enabled is only the ini snapshot taken at creation.
        if (!m_engine.OptEnabled()) {
            reason = kReasonDisabled;
            NoteOnce(&m_whyDisabled, "disabled by the control panel or the ini");
            SetStatusLocal("disabled by config/panel");
        } else if (!m_engine.Ready()) {
            reason = m_engine.GaveUp() ? kReasonGiveUp : kReasonNotReady;
            NoteOnce(&m_whyNotReady, "engine not ready yet (loading, or init failed) - passing through");
            SetStatusLocal(m_engine.GaveUp() ? "engine init FAILED (see dlssnr_dshow.log)"
                                             : "loading model...");
        } else if (m_engine.Width() != w || m_engine.Height() != h) {
            reason = kReasonSizeMismatch;
            NoteOnce(&m_whySize, "size changed since init; engine will re-init on next Pause");
            SetStatusLocal("resolution changed since init");
        } else if (!isYuv) {
            // The engine is NV12-only (src/dlssnr_engine.cpp). RGB24 / RGB32
            // frames are passed through untouched, with a reason the panel can
            // show, rather than being half-processed.
            reason = kReasonBpp;
            NoteOnce(&m_whyBpp, "input is not NV12 (engine handles NV12 only) - passing through");
            SetStatusLocal("input not NV12 - engine handles NV12 only");
        } else {
            // Straight through: NV12 in, NV12 out. No CPU colour conversion.
            const BYTE* uvIn = pSrc + (size_t)inStride * h;
            BYTE* uvOut = pDst + (size_t)outStride * h;
            done = m_engine.ProcessNv12(pSrc, inStride, uvIn, inStride,
                                        pDst, outStride, uvOut, outStride,
                                        w, h, m_needReset != 0);
            if (done) {
                m_needReset = false;
            } else {
                reason = kReasonProcessFail;
                SetStatusLocal("engine error - passing through");
            }
        }
        if (!done) {
            // pass-through (engine not ready / disabled / failure): copy entire
            // frames so the output is bit-identical to the input.
            if (inStride == outStride) {
                memcpy(pDst, pSrc, need);
            } else {
                const int rowBytes = (isYuv) ? inStride : packed;
                for (int y = 0; y < h; ++y)
                    memcpy(pDst + (size_t)y * outStride, pSrc + (size_t)y * inStride, rowBytes);
            }
        }
        *reasonOut = reason;
        return done;
    }

    // ---- streaming: the engine lives here ----
    HRESULT DoReceive(IMediaSample* pIn) {
        if (!pIn) return E_POINTER;
        // Flush / stop contract: while the graph is flushing (seek, stop, graph
        // teardown) the correct answer is E_ABORT -- return immediately instead
        // of blocking the source's streaming thread inside the engine. Without
        // this, a seek waited for a full engine call and the player stalled.
        // Diagnostic: does the graph keep pushing frames while the player says
        // it is paused? That single fact decides how a live parameter change can
        // be made visible on a frozen picture.
        if (m_state != State_Running) {
            if (m_framesWhilePaused == 0 || (m_framesWhilePaused % 30) == 0)
                LogRaw("filter: frame %d arrived while state=%d (paused=%d)",
                       m_framesWhilePaused + 1, (int)m_state, (int)State_Paused);
            ++m_framesWhilePaused;
        }
        if (m_flushing) {
            if (m_droppedFlush++ == 0) LogRaw("filter: dropping frames while flushing");
            return E_ABORT;
        }
        if (m_state == State_Stopped) {
            if (m_droppedStopped++ == 0) LogRaw("filter: dropping frames while stopped");
            return S_FALSE;   // discard, do not process
        }
        const AM_MEDIA_TYPE* mt = m_in->MediaType();
        if (!mt) return VFW_E_NOT_CONNECTED;

        int w = 0, h = 0;
        m_in->Geometry(&w, &h);
        if (w <= 0 || h <= 0) return E_UNEXPECTED;
        const bool isYuv = IsYuvType(mt);
        const int bpp    = BytesPerPixel(mt);
        const size_t need = (size_t)SampleSizeOf(mt, w, h);

        // Pick up panel changes BEFORE processing so they take effect on this
        // very frame instead of the next one.
        ApplyRequests();

        BYTE* pSrc = nullptr;
        if (FAILED(pIn->GetPointer(&pSrc)) || !pSrc) return E_UNEXPECTED;

        // A paused graph delivers nothing, so remember the timestamp of the frame on
        // screen: a later parameter edit repositions the graph to it and runs it
        // again, and the engine re-runs on the way through (see CueCurrentFrame).
        // get an output sample from our allocator
        if (!m_out->Allocator()) return VFW_E_NOT_CONNECTED;
        IMediaSample* pOut = nullptr;
        HRESULT hr = m_out->Allocator()->GetBuffer(&pOut, nullptr, nullptr, 0);
        if (FAILED(hr) || !pOut) return hr;

        BYTE* pDst = nullptr;
        hr = pOut->GetPointer(&pDst);
        if (FAILED(hr) || !pDst) { pOut->Release(); return hr; }

        if (pOut->GetSize() < (long)need) { pOut->Release(); return E_UNEXPECTED; }

        // ---- run the engine, or pass through ----
        // The engine only speaks tightly packed BGR24, so:
        //   * RGB24 input is already in that layout -- pass the pointer straight
        //     in when the stride happens to be packed (the common case), else
        //     gather the rows first.
        //   * NV12/YUV input is converted into the staging buffer, processed,
        //     then converted back. Chroma is subsampled, so "processed" YUV does
        //     not round-trip bit-exactly; that is inherent to 4:2:0, not a bug.
        int reason = kReasonNone;
        bool done = false;
        {
            // Serialized against the paused-refresh path (shared scratch buffers
            // and engine temporal history).
            EnterCriticalSection(&m_streamCs);
            done = ProcessFrame(pSrc, pDst, mt, w, h, bpp, isYuv, &reason);
            LeaveCriticalSection(&m_streamCs);
        }

        // ---- timestamps / flags ----
        REFERENCE_TIME tStart = 0, tStop = 0;
        if (SUCCEEDED(pIn->GetTime(&tStart, &tStop))) pOut->SetTime(&tStart, &tStop);
        if (SUCCEEDED(pIn->GetMediaTime(&tStart, &tStop))) pOut->SetMediaTime(&tStart, &tStop);
        pOut->SetSyncPoint(pIn->IsSyncPoint() == S_OK);
        pOut->SetPreroll(pIn->IsPreroll() == S_OK);
        pOut->SetDiscontinuity(pIn->IsDiscontinuity() == S_OK);
        pOut->SetActualDataLength((long)need);

        // Feed the panel's comparison preview while it is open (pure bookkeeping:
        // nothing here touches the player's graph).
        DlssNrPreviewFrame(pSrc, pDst, w, h, bpp, isYuv, StrideOf(w, isYuv ? 1 : bpp));

        IPin* pDown = m_out->Connected();
        if (!pDown) { pOut->Release(); return VFW_E_NOT_CONNECTED; }
        // Re-check before delivering: a flush that started while the engine was
        // running makes this sample worthless, and pushing it downstream would
        // undo the renderer's flush.
        if (m_flushing) { pOut->Release(); return E_ABORT; }
        IMemInputPin* pSink = nullptr;
        hr = pDown->QueryInterface(IID_IMemInputPin, (void**)&pSink);
        if (SUCCEEDED(hr) && pSink) hr = pSink->Receive(pOut);
        if (pSink) pSink->Release();
        pOut->Release();

        // Publish telemetry / pick up panel changes once per frame.
        if (done) {
            char buf[128];
            _snprintf_s(buf, sizeof(buf), _TRUNCATE, "ACTIVE %dx%d  %.1f ms/frame",
                        w, h, m_engine.LastProcessMs());
            SetStatusLocal(buf);
        }
        PublishTelemetry(w, h, bpp, done, reason);
        return hr;
    }

    void OnInputConnected() {
        // Remember the negotiated type on the FILTER, not just on the pin.
        //
        // The input pin clears its own media type when it is disconnected
        // (CPinBase::DisconnectInternal), and players probe the graph by
        // disconnecting and reconnecting the input several times in a row. If the
        // output's type lived only on the input pin, every probe left the output
        // reporting "0 types" and the player removed the filter from the graph.
        // Holding the last negotiated type here makes the output's answer stable
        // across the whole probe sequence.
        if (m_in && m_in->MediaType()) {
            FreeMediaType(m_lastType);
            CopyMediaType(m_lastType, m_in->MediaType());
            m_hasLastType = true;
        }
        LogRaw("input connected -> output type = %s",
               (m_in && m_in->MediaType()) ? DescribeType(m_in->MediaType()).c_str()
                                           : "(none)");
    }

    void OnInputDisconnected() {
        // The filter deliberately KEEPS m_lastType here; see OnInputConnected().
        // A transform filter's output type is its input type, and surviving the
        // disconnect is what makes the output pin usable during a player's
        // connect/probe/reconnect cycle.
        m_needReset = true;
    }
    void OnInputFlush() {
        // Forwarded from the upstream pin when the graph flushes (seek, stop).
        m_flushing = true;
        m_needReset = true;
        m_droppedFlush = 0;
        LogRaw("filter: BeginFlush (state=%d)", (int)m_state);
        ForwardFlush(true);
    }
    // Remember the segment: the paused refresh replays it, which is the sequence
    // a renderer accepts while paused (same as after a seek).
    void OnInputNewSegment(REFERENCE_TIME tStart, REFERENCE_TIME tStop, double rate) {
        m_segStart = tStart;
        m_segStop = tStop;
        m_segRate = rate;
        m_segValid = true;
        ForwardSegment(tStart, tStop, rate);
    }

    void ForwardSegment(REFERENCE_TIME tStart, REFERENCE_TIME tStop, double rate) {
        if (!m_out) return;
        m_out->NewSegment(tStart, tStop, rate);
    }

    void OnInputEndFlush() {
        m_flushing = false;
        m_needReset = true;
        m_droppedStopped = 0;
        LogRaw("filter: EndFlush (state=%d)", (int)m_state);
        ForwardFlush(false);
    }

    // The renderer must see the same flush the source sent us, otherwise it
    // keeps waiting for a sample that the flush is supposed to discard.
    void ForwardFlush(bool begin) {
        if (!m_out) return;
        if (begin) m_out->BeginFlush();
        else       m_out->EndFlush();
    }

    // The last negotiated type, retained across input disconnects.
    const AM_MEDIA_TYPE* LastType() const { return m_hasLastType ? &m_lastType : nullptr; }

    // Packed staging buffers, used only when the DirectShow stride is padded.
    bool EnsureScratch(size_t bytes) {
        if (m_scratchCap >= bytes && m_scratchIn && m_scratchOut) return true;
        BYTE* a = (BYTE*)realloc(m_scratchIn, bytes);
        if (!a) return false;
        m_scratchIn = a;
        BYTE* b = (BYTE*)realloc(m_scratchOut, bytes);
        if (!b) return false;
        m_scratchOut = b;
        m_scratchCap = bytes;
        return true;
    }

private:
    LONG          m_ref;
    FILTER_STATE  m_state;
    IFilterGraph* m_pGraph;
    IReferenceClock* m_pClock;
    CInputPin*    m_in;
    COutputPin*   m_out;
    Config        m_cfg;
    // Shared process-wide engine (see Engine::Instance). A reference, not a
    // value: every filter instance must drive the SAME NGX session.
    Engine&       m_engine;
    volatile bool m_needReset;
    // Set from Stop()/BeginFlush() (graph threads) and cleared by Pause()/Run()/
    // EndFlush(); DoReceive() checks it and answers E_ABORT while it is set.
    volatile bool m_flushing = false;
    // Diagnostics for "can a parameter change be seen while paused": how many
    // frames the graph pushes while we are not running, and how many we drop.
    volatile long m_framesWhilePaused = 0;
    volatile long m_droppedFlush = 0;
    volatile long m_droppedStopped = 0;

    // ---- the paused refresh (see StepOneFrame) ----
    volatile bool m_refreshQueued = false;
    bool          m_loggedPausedEdit = false;   // one log line per session
    volatile bool m_rebuildQueued = false;      // one preview re-run at a time
    volatile bool m_shutdown = false;
    // Last segment seen upstream; replayed before a paused refresh.
    REFERENCE_TIME m_segStart = 0, m_segStop = 0;
    double         m_segRate = 1.0;
    bool           m_segValid = false;
    // Serializes frame processing between the streaming path and the refresh.
    CRITICAL_SECTION m_streamCs{};
    bool          m_streamCsInit = false;
    BYTE*         m_scratchIn;
    BYTE*         m_scratchOut;
    size_t        m_scratchCap;
    // Last negotiated input type, retained across disconnects so the output pin
    // keeps reporting a usable type while a player probes the graph.
    AM_MEDIA_TYPE m_lastType;
    bool          m_hasLastType = false;
    bool          m_trayHooked  = false;
    // Silent pass-through is the worst failure mode: the user sees "no effect"
    // with no clue why. Log each distinct reason exactly once per session.
    bool          m_whyDisabled  = false;
    bool          m_whyBpp       = false;
    bool          m_whyNotReady  = false;
    bool          m_whySize      = false;
    // live control channel
    long          m_lastSeq      = 0;
    char          m_statusText[128] = "starting up";

    void SetStatusLocal(const char* s) {
        strncpy_s(m_statusText, sizeof(m_statusText), s, _TRUNCATE);
    }

    static void NoteOnce(bool* flag, const char* msg) {
        if (*flag) return;
        *flag = true;
        LogRaw("passthrough: %s", msg);
    }
};

void CInputPin::OnConnected()     { if (m_pFilter) m_pFilter->OnInputConnected(); }
HRESULT CInputPin::DoReceive(IMediaSample* p) { return m_pFilter ? m_pFilter->DoReceive(p) : E_UNEXPECTED; }
HRESULT CInputPin::OnBeginFlush() { if (m_pFilter) m_pFilter->OnInputFlush(); return S_OK; }
HRESULT CInputPin::OnEndFlush()   { if (m_pFilter) m_pFilter->OnInputEndFlush(); return S_OK; }
HRESULT CInputPin::OnNewSegment(REFERENCE_TIME tStart, REFERENCE_TIME tStop, double rate) {
    if (m_pFilter) m_pFilter->OnInputNewSegment(tStart, tStop, rate);
    return S_OK;
}
void    CInputPin::OnDisconnected() {
    if (m_pFilter) m_pFilter->OnInputDisconnected();
    SetAllocator(nullptr);
}

// The output pin derives its type from the input pin on every query, so a
// disconnect/reconnect probe by a player can never leave it reporting "no type".
// Defined here because CDlssNrFilter is only forward-declared at the class body.
const AM_MEDIA_TYPE* COutputPin::MediaType() const {
    if (!m_pFilter) return nullptr;
    // Prefer the input pin's live type; fall back to the type the filter
    // retained across disconnects. Both are needed: the live one is
    // authoritative while connected, the retained one keeps the output usable
    // during a player's disconnect/reconnect probing.
    CInputPin* in = m_pFilter->Input();
    const AM_MEDIA_TYPE* live = in ? in->MediaType() : nullptr;
    return live ? live : m_pFilter->LastType();
}

// ---------------------------------------------------------------------------
// IEnumPins
// ---------------------------------------------------------------------------
class CEnumPins : public IEnumPins {
public:
    CEnumPins(CDlssNrFilter* f) : m_ref(1), m_pos(0) { m_pins[0] = f->Input(); m_pins[1] = f->Output(); }
    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) {
        if (!ppv) return E_POINTER;
        if (riid == IID_IUnknown || riid == IID_IEnumPins) { *ppv = this; AddRef(); return S_OK; }
        *ppv = nullptr; return E_NOINTERFACE;
    }
    STDMETHODIMP_(ULONG) AddRef() { return InterlockedIncrement(&m_ref); }
    STDMETHODIMP_(ULONG) Release() {
        LONG r = InterlockedDecrement(&m_ref);
        if (r == 0) delete this;
        return r;
    }
    STDMETHODIMP Next(ULONG c, IPin** pp, ULONG* fetched) {
        if (!pp) return E_POINTER;
        ULONG n = 0;
        while (n < c && m_pos < 2) {
            pp[n] = m_pins[m_pos];
            pp[n]->AddRef();
            ++n; ++m_pos;
        }
        if (fetched) *fetched = n;
        return (n == c) ? S_OK : S_FALSE;
    }
    STDMETHODIMP Skip(ULONG c) { m_pos += c; return (m_pos <= 2) ? S_OK : S_FALSE; }
    STDMETHODIMP Reset() { m_pos = 0; return S_OK; }
    STDMETHODIMP Clone(IEnumPins** pp) {
        if (!pp) return E_POINTER;
        CEnumPins* p = new CEnumPins(m_pins[0], m_pins[1]);
        p->m_pos = m_pos;
        *pp = p; return S_OK;
    }
private:
    CEnumPins(IPin* a, IPin* b) : m_ref(1), m_pos(0) { m_pins[0] = a; m_pins[1] = b; }
    LONG m_ref; ULONG m_pos; IPin* m_pins[2];
};

STDMETHODIMP CDlssNrFilter::EnumPins(IEnumPins** ppEnum) {
    if (!ppEnum) return E_POINTER;
    *ppEnum = new CEnumPins(this);
    return *ppEnum ? S_OK : E_OUTOFMEMORY;
}

// ---------------------------------------------------------------------------
// class factory + COM exports
// ---------------------------------------------------------------------------
class CClassFactory : public IClassFactory {
public:
    explicit CClassFactory(const CLSID& clsid) : m_ref(1), m_clsid(clsid) {}
    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) {
        if (!ppv) return E_POINTER;
        if (riid == IID_IUnknown || riid == IID_IClassFactory) { *ppv = this; AddRef(); return S_OK; }
        *ppv = nullptr; return E_NOINTERFACE;
    }
    STDMETHODIMP_(ULONG) AddRef() { return InterlockedIncrement(&m_ref); }
    STDMETHODIMP_(ULONG) Release() {
        LONG r = InterlockedDecrement(&m_ref);
        if (r == 0) delete this;
        return r;
    }
    STDMETHODIMP CreateInstance(IUnknown* pUnkOuter, REFIID riid, void** ppv) {
        if (!ppv) return E_POINTER;
        if (pUnkOuter) return CLASS_E_NOAGGREGATION;
        IUnknown* p = nullptr;
        if (IsEqualCLSID(m_clsid, CLSID_DlssNrFilter)) {
            // Cast through the concrete interface: the filter implements two
            // IUnknown-derived interfaces, so an implicit conversion is ambiguous.
            p = static_cast<IBaseFilter*>(new CDlssNrFilter());
        } else if (IsEqualCLSID(m_clsid, CLSID_DlssNrPage)) {
            p = static_cast<IPropertyPage*>(new CDlssNrPage());
        } else {
            return CLASS_E_CLASSNOTAVAILABLE;
        }
        if (!p) return E_OUTOFMEMORY;
        HRESULT hr = p->QueryInterface(riid, ppv);
        p->Release();
        return hr;
    }
    STDMETHODIMP LockServer(BOOL bLock) {
        if (bLock) InterlockedIncrement(&g_lockCount);
        else       InterlockedDecrement(&g_lockCount);
        return S_OK;
    }
private:
    LONG  m_ref;
    CLSID m_clsid;
};

STDAPI DllGetClassObject(REFCLSID rclsid, REFIID riid, void** ppv) {
    if (!ppv) return E_POINTER;
    if (!IsEqualCLSID(rclsid, CLSID_DlssNrFilter) &&
        !IsEqualCLSID(rclsid, CLSID_DlssNrPage)) return CLASS_E_CLASSNOTAVAILABLE;
    CClassFactory* p = new CClassFactory(rclsid);
    if (!p) return E_OUTOFMEMORY;
    HRESULT hr = p->QueryInterface(riid, ppv);
    p->Release();
    return hr;
}

STDAPI DllCanUnloadNow() {
    return (g_lockCount == 0) ? S_OK : S_FALSE;
}

// ---------------------------------------------------------------------------
// registration
// ---------------------------------------------------------------------------
static HRESULT RegisterOneClsid(REFCLSID id, const wchar_t* desc, bool reg) {
    wchar_t clsid[64];
    StringFromGUID2(id, clsid, _countof(clsid));
    wchar_t key[MAX_PATH];
    _snwprintf_s(key, _countof(key), _TRUNCATE, L"CLSID\\%s", clsid);

    if (!reg) {
        RegDeleteKeyW(HKEY_CLASSES_ROOT, key);
        return S_OK;
    }
    HKEY hk = nullptr;
    if (RegCreateKeyExW(HKEY_CLASSES_ROOT, key, 0, nullptr, 0, KEY_WRITE, nullptr, &hk, nullptr) != ERROR_SUCCESS)
        return E_FAIL;
    RegSetValueExW(hk, nullptr, 0, REG_SZ, (const BYTE*)desc,
                   (DWORD)((wcslen(desc) + 1) * sizeof(wchar_t)));
    RegCloseKey(hk);

    _snwprintf_s(key, _countof(key), _TRUNCATE, L"CLSID\\%s\\InprocServer32", clsid);
    if (RegCreateKeyExW(HKEY_CLASSES_ROOT, key, 0, nullptr, 0, KEY_WRITE, nullptr, &hk, nullptr) != ERROR_SUCCESS)
        return E_FAIL;
    wchar_t dll[MAX_PATH];
    _snwprintf_s(dll, _countof(dll), _TRUNCATE, L"%s\\dlssnr_dshow.dll", g_dir);
    RegSetValueExW(hk, nullptr, 0, REG_SZ, (const BYTE*)dll,
                   (DWORD)((wcslen(dll) + 1) * sizeof(wchar_t)));
    const wchar_t* tm = L"Both";
    RegSetValueExW(hk, L"ThreadingModel", 0, REG_SZ, (const BYTE*)tm,
                   (DWORD)((wcslen(tm) + 1) * sizeof(wchar_t)));
    RegCloseKey(hk);
    return S_OK;
}

// Both COM objects must be registered: the filter itself, and the property page
// (the player's property frame CoCreateInstance()s the page by CLSID).
static HRESULT RegisterComServer(bool reg) {
    HRESULT a = RegisterOneClsid(CLSID_DlssNrFilter, kFilterName, reg);
    HRESULT b = RegisterOneClsid(CLSID_DlssNrPage, L"DLSS Neural Render Settings", reg);
    return FAILED(a) ? a : b;
}

STDAPI DllRegisterServer() {
    InitModuleDir(g_hModule);

    // These two registrations are INDEPENDENT. Writing the COM server key lives
    // under HKCR\CLSID and needs Administrator; the DirectShow filter entry is
    // written through IFilterMapper2. Do not let a failure in one skip the
    // other -- an early return there is why a re-register once silently left the
    // old media types in the filter store.
    HRESULT hrCom = RegisterComServer(true);
    if (FAILED(hrCom))
        LogRaw("register: COM server failed 0x%08X (running elevated?)", (unsigned)hrCom);

    IFilterMapper2* pMapper = nullptr;
    HRESULT hr = CoCreateInstance(CLSID_FilterMapper2, nullptr, CLSCTX_INPROC_SERVER,
                                  IID_IFilterMapper2, (void**)&pMapper);
    if (FAILED(hr)) {
        LogRaw("register: FilterMapper2 failed 0x%08X", (unsigned)hr);
        return FAILED(hrCom) ? hrCom : hr;
    }

    // Advertise NV12 FIRST: that is what hardware/software decoders emit by
    // default, so it is the type the graph builder should match on.
    REGPINTYPES inTypes[3] = { { &MEDIATYPE_Video, &MEDIASUBTYPE_NV12 },
                               { &MEDIATYPE_Video, &MEDIASUBTYPE_RGB24 },
                               { &MEDIATYPE_Video, &MEDIASUBTYPE_RGB32 } };
    REGPINTYPES outTypes[3] = { { &MEDIATYPE_Video, &MEDIASUBTYPE_NV12 },
                                { &MEDIATYPE_Video, &MEDIASUBTYPE_RGB24 },
                                { &MEDIATYPE_Video, &MEDIASUBTYPE_RGB32 } };
    REGFILTERPINS2 pins[2];
    ZeroMemory(pins, sizeof(pins));

    // Pin 0 = INPUT. No direction flag means input; REG_PINFLAG_B_RENDERER is
    // NOT set because this filter is a transform, not a renderer.
    pins[0].dwFlags      = 0;
    pins[0].cInstances   = 1;
    pins[0].nMediaTypes  = 3;
    pins[0].lpMediaType  = inTypes;
    pins[0].nMediums     = 0;
    pins[0].lpMedium     = nullptr;
    pins[0].clsPinCategory = nullptr;

    // Pin 1 = OUTPUT, and it MUST be flagged as such.
    //
    // REGFILTERPINS2 encodes pin direction in dwFlags: an output pin requires
    // REG_PINFLAG_B_OUTPUT (0x8). Registering it without the flag makes the
    // filter store describe the filter as having TWO INPUT pins and NO OUTPUT.
    // A player that enumerates media types from the registry (PotPlayer does)
    // then sees nowhere to send the frames: it connects our input, finds no
    // usable output, and removes the filter from the graph. Its logs show
    // exactly that -- our input connection succeeds and the output pin's
    // Connect() is never called.
    //
    // DirectShow's own IGraphBuilder does NOT read these flags (it asks each pin
    // its direction), which is why every hand-built test kept passing.
    pins[1].dwFlags      = REG_PINFLAG_B_OUTPUT;
    pins[1].cInstances   = 1;
    pins[1].nMediaTypes  = 3;
    pins[1].lpMediaType  = outTypes;
    pins[1].nMediums     = 0;
    pins[1].lpMedium     = nullptr;
    pins[1].clsPinCategory = nullptr;

    REGFILTER2 rf2;
    ZeroMemory(&rf2, sizeof(rf2));
    rf2.dwVersion = 2;
    // MERIT_DO_NOT_USE: never auto-inserted by Intelligent Connect. The user
    // adds it explicitly, otherwise it would hijack every graph on the box.
    rf2.dwMerit   = MERIT_DO_NOT_USE;
    rf2.cPins2    = 2;
    rf2.rgPins2   = pins;

    hr = pMapper->RegisterFilter(CLSID_DlssNrFilter, kFilterName, nullptr,
                                 &CLSID_LegacyAmFilterCategory, nullptr, &rf2);
    pMapper->Release();

    // Log what was actually registered: a wrong pin direction here is invisible
    // to IGraphBuilder-based tests and only shows up in a real player.
    LogRaw("register: pins = [0]%s (in)  [1]%s (out)",
           (pins[0].dwFlags & REG_PINFLAG_B_OUTPUT) ? "OUT" : "IN",
           (pins[1].dwFlags & REG_PINFLAG_B_OUTPUT) ? "OUT" : "IN");

    if (FAILED(hr)) { LogRaw("register: RegisterFilter failed 0x%08X", (unsigned)hr); return hr; }

    LogRaw("register: OK%s", FAILED(hrCom) ? " (filter entry only; COM key needs admin)" : "");
    // Report the COM failure if it happened, so regsvr32 surfaces it -- but only
    // after the filter entry was written, which is the part that makes the
    // filter usable.
    return FAILED(hrCom) ? hrCom : S_OK;
}

STDAPI DllUnregisterServer() {
    InitModuleDir(g_hModule);
    IFilterMapper2* pMapper = nullptr;
    if (SUCCEEDED(CoCreateInstance(CLSID_FilterMapper2, nullptr, CLSCTX_INPROC_SERVER,
                                   IID_IFilterMapper2, (void**)&pMapper))) {
        pMapper->UnregisterFilter(&CLSID_LegacyAmFilterCategory, nullptr, CLSID_DlssNrFilter);
        pMapper->Release();
    }
    RegisterComServer(false);
    LogRaw("unregister: OK");
    return S_OK;
}

// ---------------------------------------------------------------------------
BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(hModule);
#if defined(_M_X64) || defined(_M_IX86)
        {   // SSSE3 (pshufb) is used by the vectorised NV12<->BGR24 path.
            int f[4] = { 0, 0, 0, 0 };
            __cpuid(f, 1);
            g_convSSSE3 = (f[2] & (1 << 9)) != 0;
        }
#endif
        InitModuleDir(hModule);
    } else if (reason == DLL_PROCESS_DETACH) {
        CloseShared();
        if (g_logLockInit) { g_logLockInit = false; DeleteCriticalSection(&g_logLock); }
    }
    return TRUE;
}
