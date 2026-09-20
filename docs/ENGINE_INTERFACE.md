# Engine interface contract / 引擎接口契约

This filter is a thin DirectShow wrapper. All the actual neural work happens in
`dlssnr_host2.dll`, which is built in the separate
**[dlssnr-toolkit](https://github.com/CyanKe/dlssnr-toolkit)** repository.

That makes the exported functions below a **binary interface between two
repositories**, so they are documented here explicitly: changing a signature on
one side silently breaks the other.

本滤镜只是 DirectShow 外壳，真正的神经渲染全部发生在 `dlssnr_host2.dll` 里，
而它是在独立的 **dlssnr-toolkit** 仓库中编译的。因此下面这些导出函数是
**两个仓库之间的二进制接口** —— 任何一侧改了签名，另一侧会静默失效。

---

## What the filter loads

```
LoadLibraryW("<dir of dlssnr_dshow.dll>\\dlssnr_host2.dll")
```

Resolution is relative to the filter DLL's own directory (`g_dir`, filled from
`GetModuleFileNameW` in `DllMain`). Nothing is hard-coded, so the folder can be
moved or cloned anywhere.

Then `GetProcAddress` for exactly these symbols. The five the filter actually
calls are marked **required** — if one is missing, the filter logs the reason and
stays in pass-through.

| Export | Signature | Used for | Required |
| --- | --- | --- | --- |
| `dlssnr2_set_appdir` | `void (const wchar_t* dir)` | where the host finds `nvngx_dlssnr.dll`; must be called **before** the first init | **yes** |
| `dlssnr2_init` | `int (int w, int h, const wchar_t* logPath)`, returns 1 on success | create the NGX feature at a size | **yes** |
| `dlssnr2_process` | `int (unsigned char* inBgr, unsigned char* outBgr, int reset)` | one frame, BGR24 in / BGR24 out | **yes** |
| `dlssnr2_set_options` | `void (int style, float intensity, float localTone, float localStruct, float skinStruct, int autoMask, int uiCorrection)` | live parameter update | **yes** |
| `dlssnr2_get_sizes` | `void (int* outW, int* outH)` | the size the session actually runs at | **yes** |

> **`dlssnr2_process` outputs BGR24, not RGBA8.** The RGBA variant is a separate
> entry point, `dlssnr2_process_rgba(inBgr, outRgba, reset)`, which is **BGR in,
> RGBA out** — the name refers to the output format only. The DirectShow filter
> converts to/from NV12 itself and uses the plain BGR24 path.
>
> The third argument is `reset`, **not** a bit depth. Passing a pixel size there
> is a silent, hard-to-diagnose bug: the engine would treat it as a truthy flag
> and reset its temporal history on every frame.

The host also exports these, which the DirectShow filter does **not** use (they
exist for the Python app): `dlssnr2_process_rgba`, `dlssnr2_submit`,
`dlssnr2_fetch`, `dlssnr2_pending`, `dlssnr2_drain`, `dlssnr2_shutdown`,
`dlssnr2_set_preset`, `dlssnr2_set_mvec_scale`, `dlssnr2_set_aux`,
`dlssnr2_mvec_const`, `dlssnr2_aux_test`.

`dlssnr2_set_appdir` matters for the filter specifically: the filter runs inside
a **player's** process, so the host cannot assume the working directory is the
tool's folder. The filter calls it before `dlssnr2_init` to point the host at the
filter's own directory.

**Note:** parameter values are passed as **percentages 0–100**, not 0–1
fractions. The DirectShow filter and the Python app agree on this; changing it
on one side alone would scale the effect wrongly by 100×.

---

## Buffer contracts

Both `process` entry points use **tightly packed** system memory — no stride
padding. DirectShow allocates with `cbBuffer` exactly equal to the packed frame
size, and the filter repacks into scratch buffers if a downstream allocator ever
hands it a padded stride.

| Function | Input | Output |
| --- | --- | --- |
| `dlssnr2_process` | `w*h*3` bytes, BGR24 | `w*h*4` bytes, RGBA8 |
| `dlssnr2_process_rgba` | `w*h*4` bytes, RGBA8 | `w*h*4` bytes, RGBA8 |

`MEDIASUBTYPE_RGB24` in DirectShow is **B, G, R in memory** (the Windows DIB
convention — the subtype name lies). That matches `dlssnr2_process` byte for
byte, with no channel swap. This is not a coincidence to be "fixed" later.

---

## Lifetime contract: the host must stay loaded

`NVSDK_NGX_D3D12_Init_Ext` may be called **once per process**. The host keeps that
latch inside its own DLL image (`static bool ngxInited` in `dlssnr_host2.cpp`), so
unloading `dlssnr_host2.dll` and loading it again **resets the latch while NGX
itself is still initialised in the process**. The next `Init_Ext` then fails with
`0xBAD00002` (`FAIL_PlatformError`) and the engine stays dead until that process
exits.

That is exactly what a player does when it opens another file: it builds a **new
filter instance**. Therefore:

- **The filter must never `FreeLibrary` the host.** `Engine` in `dlssnr_dshow.cpp`
  is a process-wide singleton (`Engine::Instance()`) that is deliberately leaked,
  and it additionally pins the filter DLL via `GET_MODULE_HANDLE_EX_FLAG_PIN`,
  because its worker thread executes inside that module.
- An earlier revision used a **per-filter-instance** engine and called
  `FreeLibrary` from `~Engine`. Every video change then produced
  `Init_Ext fail 0xBAD00002` and permanent pass-through until the player itself
  was restarted. **Do not reintroduce that.**

**Resizing does work in-process.** While the host stays loaded, `dlssnr2_init()`
at a new size reuses the D3D12 device, drops the old feature via
`ReleaseFeatureAndTextures()` and rebuilds it at the new geometry. Measured in a
single process: 1920x1080 -> 1280x720 -> 3840x2160 -> 1920x1080, all four
succeeded. The former "one size per process / restart the player" claim was an
artefact of the unload bug, not an NGX limitation.

`dlssnr2_get_sizes` exists so the filter can report the session's real size to the
control panel instead of guessing.

**中文摘要**：NGX 的 `Init_Ext` 每进程只能调用一次，而 host 把这个标志存在自己的
DLL 映像里。过滤器**绝不可以 `FreeLibrary` host**，否则换视频时标志被清零、NGX
却仍处于已初始化状态，第二次 `Init_Ext` 会以 `0xBAD00002` 失败，直到重启播放器。
保持 host 加载后，**同进程内换分辨率是正常工作的**。

---

## Initialisation is one-shot and slow

Loading the network costs roughly **1–15 seconds** (first run uncached, then
driver-cached at about 1.3 s). Two consequences the filter is built around:

1. **Init runs on a worker thread.** Blocking `Connect()` or `Pause()` for that
   long would freeze the player. Every frame passes through untouched until
   `Ready()` flips.
2. **Init failure is latched after 3 attempts per geometry.** Retrying on every
   wake-up spun a core and flooded the log — an actual bug that was fixed. Because
   the engine now lives for the whole process, `Engine::NewSession()` (called when
   a new filter instance appears) clears that latch, so one bad file cannot
   disable DLSSNR for every later file.

`NVSDK_NGX_D3D11_Shutdown1` **hangs forever** in this configuration, so the host
never shuts NGX down. Do not add a shutdown call.

The same is true of `NvVFX_DestroyEffect` in the RTX Video path — sessions are
pooled by size and the quality level is switched in place instead.

---

## Shared-memory block: telemetry only

The control panel is a **native Win32 window inside the filter DLL**
(`src/dlssnr_ui.h`), so it needs no IPC to drive the engine -- it calls
`Engine::SetOptions()` directly. The named block still exists because the filter
publishes its live telemetry through it:

```
Local\DLSSNR_DShow_Shared_v1
```

A 364-byte packed struct with magic `0x4E534C44` (`'DLSN'`). One producer (the
filter) and one consumer (the panel, same process), with a `structSize` field so
a layout mismatch is detected rather than misread. The `SharedState` struct is
defined in `dlssnr_dshow.cpp`.

- **filter -> panel**: `heartbeat`, `framesSeen`, `framesProcessed`,
  `framesPassthrough`, `engineReady`, `engineGaveUp`, `lastReason`, `engineW/H`,
  `videoW/H`, `inputBpp`, `pid`, `lastProcessMs`, `status`
- **panel -> filter** (`req*`, `reqSeq`): **legacy and unused.** This direction
  only ever existed so the old out-of-process Python panel could push parameters.
  The in-process panel writes the ini and calls `Engine::SetOptions()` instead.
  The fields are kept because dropping them would change the struct layout for no
  benefit.

The filter applies any pending request **before** processing a frame and publishes
telemetry **after**, so a slider change lands on the very frame it is picked up.

---

## Versioning

There is no version negotiation on the host interface. Treat a change to any
signature in this file as a **breaking change** requiring `dlssnr-toolkit` and
`dlssnr-filter` to be released together. `SharedState` is private to this
repository now (filter <-> its own panel) and can change freely.
