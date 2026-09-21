// dlssnr_engine.cpp -- see dlssnr_engine.h for why this exists.
//
// Layout of one frame:
//   CPU : memcpy the two NV12 planes into 256-byte-aligned upload buffers
//   GPU : CopyTextureRegion -> yTex/uvTex
//         [compute] nv12_to_rgba  -> texIn (RGBA8)
//         NGX EvaluateFeature     (DLSSNR.Color=texIn, .Output=texOut)
//         [compute] rgba_to_nv12  -> outY/outUV
//         CopyTextureRegion -> readback buffers
//   CPU : memcpy the two planes out to the caller

#include "dlssnr_engine.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

#include "nvsdk_ngx.h"
#include "nvsdk_ngx_helpers.h"

#pragma comment(lib, "d3d12.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "advapi32.lib")

namespace dlssnr {
namespace {

// ---------------------------------------------------------------- snippet ABI
// The entry points below live in nvngx_dlssnr.dll, NOT in the SDK static lib --
// the static lib only provides GetCapabilityParameters/DestroyParameters. That
// split is why the host DLL and now this engine declare their own typedefs and
// resolve these four with GetProcAddress.
typedef NVSDK_NGX_Result(NVSDK_CONV* FnInitExt)(
    unsigned long long, const wchar_t*, ID3D12Device*, NVSDK_NGX_Version,
    const NVSDK_NGX_Parameter*);
typedef NVSDK_NGX_Result(NVSDK_CONV* FnCreateFeature)(
    ID3D12GraphicsCommandList*, NVSDK_NGX_Feature, NVSDK_NGX_Parameter*,
    NVSDK_NGX_Handle**);
typedef NVSDK_NGX_Result(NVSDK_CONV* FnEvaluateFeature)(
    ID3D12GraphicsCommandList*, const NVSDK_NGX_Handle*,
    const NVSDK_NGX_Parameter*, PFN_NVSDK_NGX_ProgressCallback);
typedef NVSDK_NGX_Result(NVSDK_CONV* FnReleaseFeature)(NVSDK_NGX_Handle*);

typedef HRESULT(WINAPI* FnD3DCompile)(LPCVOID, SIZE_T, LPCSTR,
    const D3D_SHADER_MACRO*, ID3DInclude*, LPCSTR, LPCSTR, UINT, UINT,
    ID3DBlob**, ID3DBlob**);

// ---------------------------------------------------------------------- log
static char        g_logPath[MAX_PATH] = {0};
static CRITICAL_SECTION g_logCs;
static bool        g_logCsInit = false;

static void ELog(const char* fmt, ...) {
    if (!g_logPath[0]) return;
    if (!g_logCsInit) return;
    char line[512];
    va_list ap; va_start(ap, fmt);
    _vsnprintf_s(line, sizeof(line), _TRUNCATE, fmt, ap);
    va_end(ap);
    EnterCriticalSection(&g_logCs);
    FILE* f = nullptr;
    if (fopen_s(&f, g_logPath, "a") == 0 && f) { fprintf(f, "[engine] %s\n", line); fclose(f); }
    LeaveCriticalSection(&g_logCs);
}

// --------------------------------------------------------- D3D12 utilities
static bool WaitForFence(ID3D12Fence* f, HANDLE ev, uint64_t v) {
    if (!f || !ev) return false;
    if (f->GetCompletedValue() >= v) return true;
    if (FAILED(f->SetEventOnCompletion(v, ev))) return false;
    return WaitForSingleObject(ev, INFINITE) == WAIT_OBJECT_0;
}

static void BarrierOn(ID3D12GraphicsCommandList* c, ID3D12Resource* r,
                      D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after) {
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = r;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = before;
    b.Transition.StateAfter = after;
    c->ResourceBarrier(1, &b);
}

static ID3D12Resource* CreateTex(ID3D12Device* dev, DXGI_FORMAT fmt, UINT w, UINT h,
                                 bool allowUav) {
    D3D12_HEAP_PROPERTIES hp{}; hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC d{};
    d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    d.Width = w; d.Height = h; d.DepthOrArraySize = 1; d.MipLevels = 1;
    d.Format = fmt; d.SampleDesc.Count = 1;
    d.Flags = allowUav ? D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS
                       : D3D12_RESOURCE_FLAG_NONE;
    ID3D12Resource* t = nullptr;
    if (FAILED(dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d,
            D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&t)))) return nullptr;
    return t;
}

static ID3D12Resource* CreateBuf(ID3D12Device* dev, UINT64 size, D3D12_HEAP_TYPE type,
                                 D3D12_RESOURCE_STATES state) {
    D3D12_HEAP_PROPERTIES hp{}; hp.Type = type;
    D3D12_RESOURCE_DESC d{};
    d.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    d.Width = size; d.Height = 1; d.DepthOrArraySize = 1; d.MipLevels = 1;
    d.Format = DXGI_FORMAT_UNKNOWN; d.SampleDesc.Count = 1;
    d.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ID3D12Resource* t = nullptr;
    if (FAILED(dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d,
            state, nullptr, IID_PPV_ARGS(&t)))) return nullptr;
    return t;
}

static UINT Align256(UINT v) { return (v + 255u) & ~255u; }

// ---------------------------------------------- caller-compatibility IAT hook
// nvngx_dlssnr.dll asks Windows for the file name of its CALLER's module and
// refuses to run unless that name is "nvngx.dll". Our module is
// dlssnr_dshow.dll, so we patch the snippet's GetModuleFileNameW import to lie
// about exactly that one module handle and forward everything else. This is the
// same shim dlssnr_host2.dll installs, and Init_Ext fails without it.
static HMODULE g_callerModule = nullptr;
static DWORD(WINAPI* g_origGetModuleFileNameW)(HMODULE, LPWSTR, DWORD) = nullptr;

static DWORD WINAPI HookedGetModuleFileNameW(HMODULE module, LPWSTR filename, DWORD size) {
    if (module == g_callerModule) {
        const wchar_t* A = L"nvngx.dll";
        const DWORD L = (DWORD)wcslen(A);
        if (!filename || !size) { SetLastError(ERROR_INSUFFICIENT_BUFFER); return 0; }
        if (size <= L) {
            if (size > 1) memcpy(filename, A, (size - 1) * sizeof(wchar_t));
            filename[size - 1] = L'\0';
            SetLastError(ERROR_INSUFFICIENT_BUFFER);
            return size;
        }
        wcscpy_s(filename, size, A);
        return L + 1;
    }
    if (g_origGetModuleFileNameW) return g_origGetModuleFileNameW(module, filename, size);
    SetLastError(ERROR_INVALID_FUNCTION);
    return 0;
}

static void** FindImportSlot(HMODULE module, const char* funcName) {
    auto* base = (uint8_t*)module;
    auto* dos = (IMAGE_DOS_HEADER*)base;
    if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew <= 0) return nullptr;
    auto* nt = (IMAGE_NT_HEADERS64*)(base + dos->e_lfanew);
    auto& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!dir.VirtualAddress || !dir.Size) return nullptr;
    auto* desc = (IMAGE_IMPORT_DESCRIPTOR*)(base + dir.VirtualAddress);
    auto* end = (IMAGE_IMPORT_DESCRIPTOR*)(base + dir.VirtualAddress + dir.Size);
    for (; desc < end && desc->Name; ++desc) {
        const char* lib = (const char*)(base + desc->Name);
        if (_stricmp(lib, "KERNEL32.dll") != 0 &&
            _stricmp(lib, "api-ms-win-core-libraryloader-l1-2-0.dll") != 0 &&
            _stricmp(lib, "api-ms-win-core-libraryloader-l1-1-0.dll") != 0) continue;
        if (!desc->OriginalFirstThunk || !desc->FirstThunk) continue;
        auto* nameThunk = (IMAGE_THUNK_DATA64*)(base + desc->OriginalFirstThunk);
        auto* addrThunk = (IMAGE_THUNK_DATA64*)(base + desc->FirstThunk);
        for (; nameThunk->u1.AddressOfData; ++nameThunk, ++addrThunk) {
            if (IMAGE_SNAP_BY_ORDINAL64(nameThunk->u1.Ordinal)) continue;
            auto* imp = (IMAGE_IMPORT_BY_NAME*)(base + nameThunk->u1.AddressOfData);
            if (strcmp(imp->Name, funcName) == 0) return (void**)&addrThunk->u1.Function;
        }
    }
    return nullptr;
}

static bool InstallCallerShim(HMODULE snippet) {
    auto slot = FindImportSlot(snippet, "GetModuleFileNameW");
    if (!slot) { ELog("shim: import slot not found"); return false; }
    if (*slot == (void*)&HookedGetModuleFileNameW) return true;
    HMODULE me = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                       GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       (LPCWSTR)&HookedGetModuleFileNameW, &me);
    DWORD old = 0;
    if (!VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &old)) return false;
    g_callerModule = me;
    void* orig = InterlockedExchangePointer(slot, (void*)&HookedGetModuleFileNameW);
    memcpy(&g_origGetModuleFileNameW, &orig, sizeof(orig));
    VirtualProtect(slot, sizeof(void*), old, nullptr);
    FlushInstructionCache(GetCurrentProcess(), slot, sizeof(void*));
    return g_origGetModuleFileNameW != nullptr;
}

// --------------------------------------------------------------- shaders
// Integer maths kept identical to the CPU loop that used to do this, so the
// picture does not shift. Channels are R,G,B,(A) -- that is the order the NGX
// feature expects, matching dlssnr_host2.dll's BGR2RGBA_row.
static const char* kSrcNv12ToRgba = R"HLSL(
cbuffer Params : register(b0) { uint W; uint H; };
Texture2D<float>    sY    : register(t0);
Texture2D<float2>   sUV   : register(t1);
RWTexture2D<float4> uRGBA : register(u0);

[numthreads(8,8,1)]
void nv12_to_rgba(uint3 tid : SV_DispatchThreadID) {
    if (tid.x >= W || tid.y >= H) return;
    int Y = (int)(sY.Load(int3(tid.xy, 0)) * 255.0 + 0.5);
    float2 uv = sUV.Load(int3(tid.xy >> 1, 0));
    int U = (int)(uv.x * 255.0 + 0.5);
    int V = (int)(uv.y * 255.0 + 0.5);
    int C = Y - 16, D = U - 128, E = V - 128;
    int B = clamp((298*C + 541*D + 128) >> 8, 0, 255);
    int G = clamp((298*C - 55*D - 136*E + 128) >> 8, 0, 255);
    int R = clamp((298*C + 459*E + 128) >> 8, 0, 255);
    uRGBA[tid.xy] = float4(R, G, B, 255.0) / 255.0;
}
)HLSL";

static const char* kSrcRgbaToNv12 = R"HLSL(
cbuffer Params : register(b0) { uint W; uint H; };
Texture2D<float4>   sRGB : register(t0);
RWTexture2D<float>  uY   : register(u0);
RWTexture2D<float2> uUV  : register(u1);

[numthreads(8,8,1)]
void rgba_to_y(uint3 tid : SV_DispatchThreadID) {
    if (tid.x >= W || tid.y >= H) return;
    float4 c = sRGB.Load(int3(tid.xy, 0));
    int R = (int)(saturate(c.r) * 255.0 + 0.5);
    int G = (int)(saturate(c.g) * 255.0 + 0.5);
    int B = (int)(saturate(c.b) * 255.0 + 0.5);
    int y = ((47*R + 157*G + 16*B + 128) >> 8) + 16;
    uY[tid.xy] = (float)clamp(y, 0, 255) / 255.0;
}

// One thread per chroma sample: average the 2x2 RGB block, exactly like the
// scalar loop (integer divide by however many samples existed at the edge).
[numthreads(8,8,1)]
void rgba_to_uv(uint3 tid : SV_DispatchThreadID) {
    uint cw = (W + 1u) / 2u, ch = (H + 1u) / 2u;
    if (tid.x >= cw || tid.y >= ch) return;
    int sr = 0, sg = 0, sb = 0, n = 0;
    for (int dy = 0; dy < 2; ++dy) {
        int y = (int)tid.y * 2 + dy;
        if (y >= (int)H) break;
        for (int dx = 0; dx < 2; ++dx) {
            int x = (int)tid.x * 2 + dx;
            if (x >= (int)W) break;
            float4 c = sRGB.Load(int3(x, y, 0));
            sr += (int)(saturate(c.r) * 255.0 + 0.5);
            sg += (int)(saturate(c.g) * 255.0 + 0.5);
            sb += (int)(saturate(c.b) * 255.0 + 0.5);
            ++n;
        }
    }
    if (!n) n = 1;
    int R = sr / n, G = sg / n, B = sb / n;
    int u = ((-26*R - 87*G + 112*B + 128) >> 8) + 128;
    int v = ((112*R - 102*G - 10*B + 128) >> 8) + 128;
    uUV[tid.xy] = float2((float)clamp(u, 0, 255), (float)clamp(v, 0, 255)) / 255.0;
}
)HLSL";

// --------------------------------------------------------------- state
enum { kDescCount = 9 };

struct State {
    ID3D12Device*         dev = nullptr;
    ID3D12CommandQueue*   queue = nullptr;
    ID3D12Fence*          fence = nullptr;
    HANDLE                fenceEvent = nullptr;
    uint64_t              fenceValue = 0;
    ID3D12CommandAllocator*  alloc = nullptr;
    ID3D12GraphicsCommandList* cmd = nullptr;

    ID3D12RootSignature*  rootSig = nullptr;
    ID3D12PipelineState*  psoToRgba = nullptr;
    ID3D12PipelineState*  psoY = nullptr;
    ID3D12PipelineState*  psoUV = nullptr;
    ID3D12DescriptorHeap* heap = nullptr;
    UINT                  descStride = 0;

    // frame resources
    ID3D12Resource* yTex = nullptr, * uvTex = nullptr;
    ID3D12Resource* texIn = nullptr, * texOut = nullptr;
    ID3D12Resource* outYTex = nullptr, * outUVTex = nullptr;
    ID3D12Resource* zeroMV = nullptr, * zeroDepth = nullptr;
    ID3D12Resource* upY = nullptr, * upUV = nullptr;
    ID3D12Resource* rbY = nullptr, * rbUV = nullptr;
    UINT yPitch = 0, uvPitch = 0;          // 256-aligned row pitches
    int  cw = 0, ch = 0;                   // chroma dimensions

    HMODULE d3dcompiler = nullptr;
    HMODULE snippet = nullptr;
    FnInitExt fnInitExt = nullptr;
    FnCreateFeature fnCreate = nullptr;
    FnEvaluateFeature fnEval = nullptr;
    FnReleaseFeature fnRelease = nullptr;

    NVSDK_NGX_Handle*    feature = nullptr;
    NVSDK_NGX_Parameter* params = nullptr;

    int w = 0, h = 0;
    Options opt;
    bool ready = false;
};
static State g;
static bool  g_ngxInited = false;   // NGX core is once per process, by design
static char  g_dirA[MAX_PATH] = {0};
static wchar_t g_dirW[MAX_PATH] = {0};

static D3D12_CPU_DESCRIPTOR_HANDLE DescCpu(int i) {
    D3D12_CPU_DESCRIPTOR_HANDLE h = g.heap->GetCPUDescriptorHandleForHeapStart();
    h.ptr += (SIZE_T)i * g.descStride;
    return h;
}
static D3D12_GPU_DESCRIPTOR_HANDLE DescGpu(int i) {
    D3D12_GPU_DESCRIPTOR_HANDLE h = g.heap->GetGPUDescriptorHandleForHeapStart();
    h.ptr += (UINT64)i * g.descStride;
    return h;
}

static void MakeSrv(ID3D12Resource* r, DXGI_FORMAT fmt, int slot) {
    D3D12_SHADER_RESOURCE_VIEW_DESC d{};
    d.Format = fmt;
    d.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    d.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    d.Texture2D.MipLevels = 1;
    g.dev->CreateShaderResourceView(r, &d, DescCpu(slot));
}
static void MakeUav(ID3D12Resource* r, DXGI_FORMAT fmt, int slot) {
    D3D12_UNORDERED_ACCESS_VIEW_DESC d{};
    d.Format = fmt;
    d.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    g.dev->CreateUnorderedAccessView(r, nullptr, &d, DescCpu(slot));
}


// --------------------------------------------------------------- builders
static void CopyToTex(ID3D12GraphicsCommandList* c, ID3D12Resource* tex,
                      ID3D12Resource* buf, UINT w, UINT h, UINT pitch, DXGI_FORMAT fmt) {
    D3D12_TEXTURE_COPY_LOCATION dst{};
    dst.pResource = tex;
    dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    dst.SubresourceIndex = 0;
    D3D12_TEXTURE_COPY_LOCATION src{};
    src.pResource = buf;
    src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    src.PlacedFootprint.Footprint.Width = w;
    src.PlacedFootprint.Footprint.Height = h;
    src.PlacedFootprint.Footprint.Depth = 1;
    src.PlacedFootprint.Footprint.RowPitch = pitch;
    src.PlacedFootprint.Footprint.Format = fmt;
    c->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
}

static void CopyFromTex(ID3D12GraphicsCommandList* c, ID3D12Resource* buf,
                        ID3D12Resource* tex, UINT w, UINT h, UINT pitch, DXGI_FORMAT fmt) {
    D3D12_TEXTURE_COPY_LOCATION dst{};
    dst.pResource = buf;
    dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dst.PlacedFootprint.Footprint.Width = w;
    dst.PlacedFootprint.Footprint.Height = h;
    dst.PlacedFootprint.Footprint.Depth = 1;
    dst.PlacedFootprint.Footprint.RowPitch = pitch;
    dst.PlacedFootprint.Footprint.Format = fmt;
    D3D12_TEXTURE_COPY_LOCATION src{};
    src.pResource = tex;
    src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    src.SubresourceIndex = 0;
    c->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
}

// Copy one plane between a caller buffer and a 256-byte-aligned staging pitch.
static void CopyRows(const uint8_t* src, int srcStride, uint8_t* dst, UINT dstPitch,
                     UINT rowBytes, UINT rows) {
    if ((UINT)srcStride == dstPitch) {
        memcpy(dst, src, (size_t)dstPitch * rows);
        return;
    }
    for (UINT y = 0; y < rows; ++y)
        memcpy(dst + (size_t)y * dstPitch, src + (size_t)y * srcStride, rowBytes);
}

static bool CreateRootSigAndPipelines() {
    // b0 = { W, H }; four one-descriptor tables: t0, t1, u0, u1.
    D3D12_DESCRIPTOR_RANGE ranges[4] = {};
    ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV; ranges[0].NumDescriptors = 1; ranges[0].BaseShaderRegister = 0;
    ranges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV; ranges[1].NumDescriptors = 1; ranges[1].BaseShaderRegister = 1;
    ranges[2].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV; ranges[2].NumDescriptors = 1; ranges[2].BaseShaderRegister = 0;
    ranges[3].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV; ranges[3].NumDescriptors = 1; ranges[3].BaseShaderRegister = 1;

    D3D12_ROOT_PARAMETER rp[5] = {};
    rp[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    rp[0].Constants.ShaderRegister = 0;
    rp[0].Constants.Num32BitValues = 2;
    rp[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    for (int i = 0; i < 4; ++i) {
        rp[1 + i].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        rp[1 + i].DescriptorTable.NumDescriptorRanges = 1;
        rp[1 + i].DescriptorTable.pDescriptorRanges = &ranges[i];
        rp[1 + i].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    }
    D3D12_ROOT_SIGNATURE_DESC rs{};
    rs.NumParameters = 5;
    rs.pParameters = rp;
    rs.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;

    ID3DBlob* sig = nullptr; ID3DBlob* err = nullptr;
    if (FAILED(D3D12SerializeRootSignature(&rs, D3D_ROOT_SIGNATURE_VERSION_1, &sig, &err))) {
        ELog("SerializeRootSignature failed: %s", err ? (const char*)err->GetBufferPointer() : "?");
        if (err) err->Release();
        return false;
    }
    HRESULT hr = g.dev->CreateRootSignature(0, sig->GetBufferPointer(), sig->GetBufferSize(),
                                            IID_PPV_ARGS(&g.rootSig));
    sig->Release();
    if (FAILED(hr)) { ELog("CreateRootSignature 0x%08X", (unsigned)hr); return false; }

    // d3dcompiler is loaded dynamically on purpose: a missing d3dcompiler_47.dll
    // must make the engine unavailable, never make the filter fail to load.
    g.d3dcompiler = LoadLibraryW(L"d3dcompiler_47.dll");
    if (!g.d3dcompiler) { ELog("d3dcompiler_47.dll missing"); return false; }
    FnD3DCompile compile = (FnD3DCompile)GetProcAddress(g.d3dcompiler, "D3DCompile");
    if (!compile) { ELog("D3DCompile not found"); return false; }

    struct { const char* src; const char* entry; ID3D12PipelineState** out; const char* name; } jobs[] = {
        { kSrcNv12ToRgba, "nv12_to_rgba", &g.psoToRgba, "nv12_to_rgba" },
        { kSrcRgbaToNv12, "rgba_to_y",    &g.psoY,      "rgba_to_y" },
        { kSrcRgbaToNv12, "rgba_to_uv",   &g.psoUV,     "rgba_to_uv" },
    };
    for (auto& j : jobs) {
        ID3DBlob* code = nullptr; ID3DBlob* e2 = nullptr;
        HRESULT h = compile(j.src, strlen(j.src), j.name, nullptr, nullptr, j.entry,
                            "cs_5_0", 0, 0, &code, &e2);
        if (FAILED(h)) {
            ELog("compile %s failed: %s", j.name, e2 ? (const char*)e2->GetBufferPointer() : "?");
            if (e2) e2->Release();
            return false;
        }
        D3D12_COMPUTE_PIPELINE_STATE_DESC pd{};
        pd.pRootSignature = g.rootSig;
        pd.CS.pShaderBytecode = code->GetBufferPointer();
        pd.CS.BytecodeLength = code->GetBufferSize();
        h = g.dev->CreateComputePipelineState(&pd, IID_PPV_ARGS(j.out));
        code->Release();
        if (FAILED(h)) { ELog("CreateComputePipelineState(%s) 0x%08X", j.name, (unsigned)h); return false; }
    }
    return true;
}

static bool CreateFrameResources(int w, int h) {
    const int cw = (w + 1) / 2, ch = (h + 1) / 2;
    g.w = w; g.h = h; g.cw = cw; g.ch = ch;
    g.yPitch  = Align256((UINT)w);
    g.uvPitch = Align256((UINT)cw * 2);

    g.yTex     = CreateTex(g.dev, DXGI_FORMAT_R8_UNORM,     w, h, true);
    g.uvTex    = CreateTex(g.dev, DXGI_FORMAT_R8G8_UNORM,   cw, ch, true);
    g.texIn    = CreateTex(g.dev, DXGI_FORMAT_R8G8B8A8_UNORM, w, h, true);
    g.texOut   = CreateTex(g.dev, DXGI_FORMAT_R8G8B8A8_UNORM, w, h, true);
    g.outYTex  = CreateTex(g.dev, DXGI_FORMAT_R8_UNORM,     w, h, true);
    g.outUVTex = CreateTex(g.dev, DXGI_FORMAT_R8G8_UNORM,   cw, ch, true);
    g.zeroMV   = CreateTex(g.dev, DXGI_FORMAT_R16G16_FLOAT, w, h, true);
    g.zeroDepth= CreateTex(g.dev, DXGI_FORMAT_R32_FLOAT,    w, h, true);

    g.upY  = CreateBuf(g.dev, (UINT64)g.yPitch * h,      D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
    g.upUV = CreateBuf(g.dev, (UINT64)g.uvPitch * ch,    D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
    g.rbY  = CreateBuf(g.dev, (UINT64)g.yPitch * h,      D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);
    g.rbUV = CreateBuf(g.dev, (UINT64)g.uvPitch * ch,    D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);

    if (!g.yTex || !g.uvTex || !g.texIn || !g.texOut || !g.outYTex || !g.outUVTex ||
        !g.zeroMV || !g.zeroDepth || !g.upY || !g.upUV || !g.rbY || !g.rbUV) {
        ELog("frame resource allocation failed at %dx%d", w, h);
        return false;
    }

    // Descriptor slots, fixed layout:
    //   0 SRV yTex   1 SRV uvTex   2 UAV texIn   3 SRV texIn
    //   4 UAV outY   5 UAV outUV    6 UAV zeroMV 7 UAV zeroDepth
    //   8 SRV texOut  <-- the RGBA8 -> NV12 pass reads THIS, not texIn
    D3D12_DESCRIPTOR_HEAP_DESC hd{};
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    hd.NumDescriptors = kDescCount;
    hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    if (FAILED(g.dev->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&g.heap)))) { ELog("descriptor heap failed"); return false; }
    g.descStride = g.dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

    MakeSrv(g.yTex,     DXGI_FORMAT_R8_UNORM,           0);
    MakeSrv(g.uvTex,    DXGI_FORMAT_R8G8_UNORM,         1);
    MakeUav(g.texIn,    DXGI_FORMAT_R8G8B8A8_UNORM,     2);
    MakeSrv(g.texIn,    DXGI_FORMAT_R8G8B8A8_UNORM,     3);
    MakeUav(g.outYTex,  DXGI_FORMAT_R8_UNORM,           4);
    MakeUav(g.outUVTex, DXGI_FORMAT_R8G8_UNORM,         5);
    MakeUav(g.zeroMV,   DXGI_FORMAT_R16G16_FLOAT,       6);
    MakeUav(g.zeroDepth,DXGI_FORMAT_R32_FLOAT,          7);
    MakeSrv(g.texOut,   DXGI_FORMAT_R8G8B8A8_UNORM,     8);

    // Deterministic zero guidance: the host DLL never wrote these, which leaves
    // the network reading whatever was in VRAM. Zeroing is one clear pass.
    g.alloc->Reset();
    g.cmd->Reset(g.alloc, nullptr);
    BarrierOn(g.cmd, g.zeroMV,    D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    BarrierOn(g.cmd, g.zeroDepth, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    const float z4[4] = {0,0,0,0};
    g.cmd->ClearUnorderedAccessViewFloat(DescGpu(6), DescCpu(6), g.zeroMV, z4, 0, nullptr);
    g.cmd->ClearUnorderedAccessViewFloat(DescGpu(7), DescCpu(7), g.zeroDepth, z4, 0, nullptr);
    BarrierOn(g.cmd, g.zeroMV,    D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON);
    BarrierOn(g.cmd, g.zeroDepth, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON);
    g.cmd->Close();
    ID3D12CommandList* lists[] = { g.cmd };
    g.queue->ExecuteCommandLists(1, lists);
    g.queue->Signal(g.fence, ++g.fenceValue);
    if (!WaitForFence(g.fence, g.fenceEvent, g.fenceValue)) { ELog("zero-clear fence failed"); return false; }
    return true;
}


// ------------------------------------------------------------ NGX bring-up
// Identifiers the DLSS NR snippet build expects (same values the toolkit host
// uses; Magpie uses them too).
static const char* kProjectId = "7c134ab9-9677-4af5-a2b2-bca943350861";
static const unsigned long long kAppId = 0x0876232CULL;

static bool CreateDeviceQueueAndCmd() {
    IDXGIFactory6* factory = nullptr;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) { ELog("CreateDXGIFactory1 failed"); return false; }
    IDXGIAdapter1* adapter = nullptr;
    for (UINT i = 0; factory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; ++i) {
        DXGI_ADAPTER_DESC d{};
        adapter->GetDesc(&d);
        if (d.VendorId == 0x10DE) break;             // NVIDIA
        adapter->Release(); adapter = nullptr;
    }
    if (!adapter) { factory->Release(); ELog("no NVIDIA adapter"); return false; }
    HRESULT hr = D3D12CreateDevice(adapter, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&g.dev));
    adapter->Release(); factory->Release();
    if (FAILED(hr)) { ELog("D3D12CreateDevice 0x%08X", (unsigned)hr); return false; }

    D3D12_COMMAND_QUEUE_DESC qd{};
    qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    if (FAILED(g.dev->CreateCommandQueue(&qd, IID_PPV_ARGS(&g.queue)))) { ELog("CreateCommandQueue failed"); return false; }
    if (FAILED(g.dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g.fence)))) { ELog("CreateFence failed"); return false; }
    g.fenceEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!g.fenceEvent) { ELog("CreateEventW failed"); return false; }
    if (FAILED(g.dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&g.alloc))) ||
        FAILED(g.dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, g.alloc, nullptr, IID_PPV_ARGS(&g.cmd)))) {
        ELog("command allocator/list failed"); return false;
    }
    g.cmd->Close();
    return true;
}

static bool TryInitNgx() {
    if (g_ngxInited) return true;
    wchar_t dll[MAX_PATH];
    swprintf_s(dll, L"%s\\nvngx_dlssnr.dll", g_dirW);
    // LOAD_WITH_ALTERED_SEARCH_PATH matters: nvngx_dlssnr.dll pulls in its own
    // dependencies (nvngxruntime.dll) from the SAME folder, and a plain
    // LoadLibrary with a full path searches the player's folder instead, which
    // fails with ERROR_MOD_NOT_FOUND (126). This flag makes the search start in
    // the loaded DLL's own directory.
    g.snippet = LoadLibraryExW(dll, nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!g.snippet) {
        ELog("LoadLibraryExW(%ls) failed err=%u", dll, GetLastError());
        return false;
    }
    g.fnInitExt = (FnInitExt)GetProcAddress(g.snippet, "NVSDK_NGX_D3D12_Init_Ext");
    g.fnCreate  = (FnCreateFeature)GetProcAddress(g.snippet, "NVSDK_NGX_D3D12_CreateFeature");
    g.fnEval    = (FnEvaluateFeature)GetProcAddress(g.snippet, "NVSDK_NGX_D3D12_EvaluateFeature");
    g.fnRelease = (FnReleaseFeature)GetProcAddress(g.snippet, "NVSDK_NGX_D3D12_ReleaseFeature");
    if (!g.fnInitExt || !g.fnCreate || !g.fnEval) { ELog("snippet exports missing"); return false; }

    const wchar_t* paths[1] = { g_dirW };
    NVSDK_NGX_FeatureCommonInfo fi{};
    fi.PathListInfo.Path = paths;
    fi.PathListInfo.Length = 1;

    NVSDK_NGX_Result r = NVSDK_NGX_D3D12_Init_with_ProjectID(
        kProjectId, NVSDK_NGX_ENGINE_TYPE_CUSTOM, "dlssnr-filter",
        g_dirW, g.dev, &fi, NVSDK_NGX_Version_API);
    if (r != NVSDK_NGX_Result_Success) { ELog("Init_with_ProjectID 0x%08X", (unsigned)r); return false; }

    // Must come between the two Init calls: the snippet resolves its caller's
    // module name during Init_Ext and rejects anything that is not nvngx.dll.
    if (!InstallCallerShim(g.snippet)) { ELog("caller shim failed"); return false; }

    r = g.fnInitExt(kAppId, g_dirW, g.dev, NVSDK_NGX_Version_API, nullptr);
    if (r != NVSDK_NGX_Result_Success) { ELog("Init_Ext 0x%08X", (unsigned)r); return false; }
    g_ngxInited = true;
    ELog("NGX core + DLSSNR snippet initialised");
    return true;
}

static bool CreateFeatureAt(int w, int h) {
    if (NVSDK_NGX_D3D12_GetCapabilityParameters(&g.params) != NVSDK_NGX_Result_Success || !g.params) {
        ELog("GetCapabilityParameters failed"); return false;
    }
    g.params->Set(NVSDK_NGX_Parameter_Width,  (uint32_t)w);
    g.params->Set(NVSDK_NGX_Parameter_Height, (uint32_t)h);
    g.params->Set("DLSSNR.Width",        (uint32_t)w);
    g.params->Set("DLSSNR.Height",       (uint32_t)h);
    g.params->Set("DLSSNR.InputWidth",   (uint32_t)w);
    g.params->Set("DLSSNR.InputHeight",  (uint32_t)h);
    g.params->Set("DLSSNR.OutputWidth",  (uint32_t)w);
    g.params->Set("DLSSNR.OutputHeight", (uint32_t)h);
    g.params->Set("DLSSNR.Upscaling",    0u);
    g.params->Set("DLSSNR.Scale",        1.0f);
    g.params->Set("DLSSNR.ScalingRatio", 1.0f);
    g.params->Set("DLSSNR.Hint.Render.Preset", 0);
    g.params->Set("DLSSNR.Style",        g.opt.style);
    g.params->Set("DLSSNR.Intensity",    g.opt.intensity);
    g.params->Set("DLSSNR.LocalToneStrength",      g.opt.localTone);
    g.params->Set("DLSSNR.LocalStructureStrength", g.opt.localStruct);
    g.params->Set("DLSSNR.SkinStructureStrength",  g.opt.skinStruct);
    g.params->Set("DLSSNR.UseAutoMask",  g.opt.autoMask);
    g.params->Set("DLSSNR.UICorrection", g.opt.uiCorrection);
    g.params->Set(NVSDK_NGX_Parameter_CreationNodeMask,   1u);
    g.params->Set(NVSDK_NGX_Parameter_VisibilityNodeMask, 1u);
    g.params->Set(NVSDK_NGX_Parameter_PerfQualityValue, (int)NVSDK_NGX_PerfQuality_Value_Balanced);

    ID3D12CommandAllocator* a = nullptr;
    ID3D12GraphicsCommandList* c = nullptr;
    if (FAILED(g.dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&a))) ||
        FAILED(g.dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, a, nullptr, IID_PPV_ARGS(&c)))) {
        ELog("command list for CreateFeature failed"); return false;
    }
    NVSDK_NGX_Result r = g.fnCreate(c, (NVSDK_NGX_Feature)18, g.params, &g.feature);
    c->Close();
    ID3D12CommandList* lists[] = { c };
    g.queue->ExecuteCommandLists(1, lists);
    g.queue->Signal(g.fence, ++g.fenceValue);
    WaitForFence(g.fence, g.fenceEvent, g.fenceValue);
    a->Release(); c->Release();
    if (r != NVSDK_NGX_Result_Success || !g.feature) { ELog("CreateFeature 0x%08X", (unsigned)r); return false; }
    return true;
}

}  // namespace

// ------------------------------------------------------------- public API
bool Init(int width, int height, const wchar_t* moduleDir, const char* logPath) {
    if (!g_logCsInit) { InitializeCriticalSection(&g_logCs); g_logCsInit = true; }
    if (logPath && logPath[0]) strncpy_s(g_logPath, sizeof(g_logPath), logPath, _TRUNCATE);
    if (moduleDir && moduleDir[0]) wcsncpy_s(g_dirW, moduleDir, _TRUNCATE);
    if (!g_dirW[0]) return false;
    if (width <= 0 || height <= 0) return false;

    if (!g.dev && !CreateDeviceQueueAndCmd()) return false;
    if (!g.rootSig && !CreateRootSigAndPipelines()) return false;

    Shutdown();                                   // drop any previous geometry
    if (!TryInitNgx()) return false;
    if (!CreateFrameResources(width, height)) return false;
    if (!CreateFeatureAt(width, height)) return false;

    g.ready = true;
    ELog("engine ready at %dx%d", width, height);
    return true;
}

void Shutdown() {
    if (!g.dev) return;
    if (g.queue && g.fence && g.fenceValue) {
        g.queue->Signal(g.fence, g.fenceValue);
        WaitForFence(g.fence, g.fenceEvent, g.fenceValue);
    }
    if (g.feature) {
        if (g.fnRelease) g.fnRelease(g.feature);
        else NVSDK_NGX_D3D12_ReleaseFeature(g.feature);
        g.feature = nullptr;
    }
    if (g.params) { NVSDK_NGX_D3D12_DestroyParameters(g.params); g.params = nullptr; }
    g.ready = false;

    ID3D12Resource* res[] = { g.yTex, g.uvTex, g.texIn, g.texOut, g.outYTex, g.outUVTex,
                              g.zeroMV, g.zeroDepth, g.upY, g.upUV, g.rbY, g.rbUV };
    for (auto*& r : res) { if (r) { r->Release(); r = nullptr; } }
    if (g.heap) { g.heap->Release(); g.heap = nullptr; }
    // The device, the queue and the NGX core stay alive for the life of the
    // process: NGX may only be initialised once, and a player builds a fresh
    // filter instance for every file it opens.
}

bool Ready()  { return g.ready; }
int  Width()  { return g.w; }
int  Height() { return g.h; }

void SetOptions(const Options& o) {
    g.opt = o;
    if (!g.params) return;
    g.params->Set("DLSSNR.Style",                   g.opt.style);
    g.params->Set("DLSSNR.Intensity",               g.opt.intensity);
    g.params->Set("DLSSNR.LocalToneStrength",       g.opt.localTone);
    g.params->Set("DLSSNR.LocalStructureStrength",  g.opt.localStruct);
    g.params->Set("DLSSNR.SkinStructureStrength",   g.opt.skinStruct);
    g.params->Set("DLSSNR.UseAutoMask",             g.opt.autoMask);
    g.params->Set("DLSSNR.UICorrection",            g.opt.uiCorrection);
}

bool ProcessNv12(const uint8_t* inY,  int inYStride,
                 const uint8_t* inUV, int inUVStride,
                 uint8_t* outY,       int outYStride,
                 uint8_t* outUV,      int outUVStride,
                 bool reset) {
    if (!g.ready || !inY || !inUV || !outY || !outUV) return false;
    const UINT W = (UINT)g.w, H = (UINT)g.h;
    const UINT cw = (UINT)g.cw, ch = (UINT)g.ch;

    // ---- CPU: stage the two planes into 256-byte-aligned upload buffers ----
    D3D12_RANGE noRead{ 0, 0 };
    void* p = nullptr;
    if (FAILED(g.upY->Map(0, &noRead, &p))) return false;
    CopyRows(inY, inYStride, (uint8_t*)p, g.yPitch, W, H);
    g.upY->Unmap(0, nullptr);
    if (FAILED(g.upUV->Map(0, &noRead, &p))) return false;
    CopyRows(inUV, inUVStride, (uint8_t*)p, g.uvPitch, cw * 2, ch);
    g.upUV->Unmap(0, nullptr);

    // ---- record the whole frame ----
    if (FAILED(g.alloc->Reset()) || FAILED(g.cmd->Reset(g.alloc, nullptr))) return false;
    ID3D12DescriptorHeap* heaps[] = { g.heap };
    g.cmd->SetDescriptorHeaps(1, heaps);
    g.cmd->SetComputeRootSignature(g.rootSig);
    const UINT dims[2] = { W, H };
    g.cmd->SetComputeRoot32BitConstants(0, 2, dims, 0);

    // NV12 planes -> textures
    BarrierOn(g.cmd, g.yTex, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);
    CopyToTex(g.cmd, g.yTex, g.upY, W, H, g.yPitch, DXGI_FORMAT_R8_UNORM);
    BarrierOn(g.cmd, g.yTex, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    BarrierOn(g.cmd, g.uvTex, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);
    CopyToTex(g.cmd, g.uvTex, g.upUV, cw, ch, g.uvPitch, DXGI_FORMAT_R8G8_UNORM);
    BarrierOn(g.cmd, g.uvTex, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

    // ---- compute: NV12 -> RGBA8 ----
    BarrierOn(g.cmd, g.texIn, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    g.cmd->SetPipelineState(g.psoToRgba);
    g.cmd->SetComputeRootDescriptorTable(1, DescGpu(0));   // t0 = Y
    g.cmd->SetComputeRootDescriptorTable(2, DescGpu(1));   // t1 = UV
    g.cmd->SetComputeRootDescriptorTable(3, DescGpu(2));   // u0 = texIn
    g.cmd->SetComputeRootDescriptorTable(4, DescGpu(2));   // u1 unused but must exist
    g.cmd->Dispatch((W + 7) / 8, (H + 7) / 8, 1);
    BarrierOn(g.cmd, g.texIn, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

    // ---- the neural pass ----
    BarrierOn(g.cmd, g.texOut,   D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    BarrierOn(g.cmd, g.zeroMV,   D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    BarrierOn(g.cmd, g.zeroDepth,D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

    g.params->Set("DLSSNR.Color",  g.texIn);
    g.params->Set("DLSSNR.Output", g.texOut);
    g.params->Set("DLSSNR.MVec",   g.zeroMV);
    g.params->Set("DLSSNR.Depth",  g.zeroDepth);
    g.params->Set("DLSSNR.Reset",  reset ? 1 : 0);
    g.params->Set("DLSSNR.ColorSubrectBaseX", 0u);
    g.params->Set("DLSSNR.ColorSubrectBaseY", 0u);
    g.params->Set("DLSSNR.ColorSubrectWidth",  W);
    g.params->Set("DLSSNR.ColorSubrectHeight", H);
    g.params->Set("DLSSNR.OutputSubrectBaseX", 0u);
    g.params->Set("DLSSNR.OutputSubrectBaseY", 0u);
    g.params->Set("DLSSNR.OutputSubrectWidth",  W);
    g.params->Set("DLSSNR.OutputSubrectHeight", H);
    g.params->Set("DLSSNR.MVecSubrectBaseX", 0u);
    g.params->Set("DLSSNR.MVecSubrectBaseY", 0u);
    g.params->Set("DLSSNR.MVecSubrectWidth",  W);
    g.params->Set("DLSSNR.MVecSubrectHeight", H);
    g.params->Set("DLSSNR.DepthSubrectBaseX", 0u);
    g.params->Set("DLSSNR.DepthSubrectBaseY", 0u);
    g.params->Set("DLSSNR.DepthSubrectWidth",  W);
    g.params->Set("DLSSNR.DepthSubrectHeight", H);
    g.params->Set("DLSSNR.MVecScaleX", 1.0f);
    g.params->Set("DLSSNR.MVecScaleY", 1.0f);
    g.params->Set("DLSSNR.DepthInverted", 1);
    g.params->Set("DLSSNR.Enabled", 1);

    NVSDK_NGX_Result r = g.fnEval(g.cmd, g.feature, g.params, nullptr);
    if (r != NVSDK_NGX_Result_Success) {
        ELog("EvaluateFeature 0x%08X at %ux%u", (unsigned)r, W, H);
        g.cmd->Close();
        return false;
    }
    BarrierOn(g.cmd, g.texOut,    D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    BarrierOn(g.cmd, g.zeroMV,    D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON);
    BarrierOn(g.cmd, g.zeroDepth, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON);

    // ---- compute: RGBA8 -> NV12 ----
    BarrierOn(g.cmd, g.outYTex,  D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    BarrierOn(g.cmd, g.outUVTex, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    g.cmd->SetPipelineState(g.psoY);
    // t0 is the NEURAL OUTPUT (slot 8 = SRV texOut). Binding slot 3 here --
    // which is texIn -- silently turns this pass into "convert the input back",
    // i.e. the filter looks like it works and has no effect at all.
    g.cmd->SetComputeRootDescriptorTable(1, DescGpu(8));   // t0 = texOut
    g.cmd->SetComputeRootDescriptorTable(2, DescGpu(8));   // t1 unused but must exist
    g.cmd->SetComputeRootDescriptorTable(3, DescGpu(4));   // u0 = outY
    g.cmd->SetComputeRootDescriptorTable(4, DescGpu(5));   // u1 = outUV
    g.cmd->Dispatch((W + 7) / 8, (H + 7) / 8, 1);
    g.cmd->SetPipelineState(g.psoUV);
    g.cmd->Dispatch((cw + 7) / 8, (ch + 7) / 8, 1);
    BarrierOn(g.cmd, g.outYTex,  D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
    BarrierOn(g.cmd, g.outUVTex, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);

    CopyFromTex(g.cmd, g.rbY,  g.outYTex,  W,  H,  g.yPitch,  DXGI_FORMAT_R8_UNORM);
    CopyFromTex(g.cmd, g.rbUV, g.outUVTex, cw, ch, g.uvPitch, DXGI_FORMAT_R8G8_UNORM);

    // back to COMMON so the next frame starts from a known state
    BarrierOn(g.cmd, g.yTex,     D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON);
    BarrierOn(g.cmd, g.uvTex,    D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON);
    BarrierOn(g.cmd, g.texIn,    D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON);
    BarrierOn(g.cmd, g.texOut,   D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON);
    BarrierOn(g.cmd, g.outYTex,  D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON);
    BarrierOn(g.cmd, g.outUVTex, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON);
    g.cmd->Close();

    ID3D12CommandList* lists[] = { g.cmd };
    g.queue->ExecuteCommandLists(1, lists);
    g.queue->Signal(g.fence, ++g.fenceValue);
    if (!WaitForFence(g.fence, g.fenceEvent, g.fenceValue)) { ELog("frame fence failed"); return false; }

    // ---- CPU: read the two planes back out ----
    D3D12_RANGE readY{ 0, (SIZE_T)g.yPitch * H };
    if (FAILED(g.rbY->Map(0, &readY, &p))) return false;
    CopyRows((const uint8_t*)p, (int)g.yPitch, outY, (UINT)outYStride, W, H);
    g.rbY->Unmap(0, nullptr);

    D3D12_RANGE readUV{ 0, (SIZE_T)g.uvPitch * ch };
    if (FAILED(g.rbUV->Map(0, &readUV, &p))) return false;
    CopyRows((const uint8_t*)p, (int)g.uvPitch, outUV, (UINT)outUVStride, cw * 2, ch);
    g.rbUV->Unmap(0, nullptr);
    return true;
}

}  // namespace dlssnr
