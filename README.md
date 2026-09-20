# dlssnr-filter

**一个 DirectShow 滤镜，把 NVIDIA 的神经渲染网络（DLSS "NR"）接进任意播放器，
边看边调参数。**

在 PotPlayer / MPC-BE / MPC-HC 里挂上它，播放的视频会逐帧经过神经降噪 / 去块 / 细节重建；
配一个实时控制面板，能在播放中用滑块调强度、局部色调、局部结构，**改完立刻生效，不用重启播放器**。

- 🎬 **即插即用** —— 挂进播放器就能用，不需要写脚本、不需要转码
- 🎚️ **实时调参** —— 共享内存通道，播放中拖动滑块即时生效
- 🔍 **状态可见** —— 面板显示心跳、帧计数、每帧耗时，以及"为什么现在没在处理"的人话原因
- 🧯 **绝不破坏播放** —— 引擎缺失、初始化失败、尺寸不支持，一律**降级为直通**，播放器不会崩
- 📦 **零第三方依赖** —— 只链接 `ole32` / `ADVAPI32` / `KERNEL32`，不需要 ATL、不需要 NVIDIA SDK

---

## 它需要什么

```
app\dlssnr_dshow.dll     ← 本仓库编译产物（含托盘图标 + 控制面板）
app\dlssnr_dshow.ini     ← 配置
app\dlssnr_host2.dll     ← ★ 引擎宿主，需要另外获取（见下）
app\nvngx_dlssnr.dll     ← ★ NVIDIA 运行库，需要自行准备
```

后两个文件**本仓库不提供**：

| 文件 | 从哪来 |
|---|---|
| `dlssnr_host2.dll` | 我们自己的代码（MIT）。从 **[dlssnr-toolkit](https://github.com/CyanKe/dlssnr-toolkit)** 的 Release 下载，或自行编译 |
| `nvngx_dlssnr.dll` | NVIDIA 的 DLSS「NR」网络。**没有官方公开下载渠道**，本项目开发所用是社区构建版；请自行取得 |

> ⚠️ **本仓库不含、也**不得**再分发任何 NVIDIA 二进制。**
> 详见 [`docs/THIRD_PARTY.md`](docs/THIRD_PARTY.md)。

**缺少引擎时滤镜照常工作** —— 它只是逐帧直通，播放器完全不受影响。日志会说明原因。

---

## 安装

### 1. 编译

需要 Visual Studio 2022（C++ 工作负载），**不需要任何 NVIDIA SDK**：

```
tools\build.bat
```

产物是 `app\dlssnr_dshow.dll`。

### 2. 放置引擎

把 `dlssnr_host2.dll`（来自 dlssnr-toolkit）和 `nvngx_dlssnr.dll`
放到**与 `dlssnr_dshow.dll` 同一目录**（即 `app\`）。

### 3. 注册（需管理员）

```
tools\register_filter.bat
```

反注册：`tools\register_filter.bat /u`

### 4. 挂进播放器

以 **MPC-BE** 为例：`选项 → 扩展滤镜 → 添加滤镜` 选 `app\dlssnr_dshow.dll`，设为 **首选**。

以 **PotPlayer** 为例：`选项 → 滤镜 → 滤镜管理器 → 添加外部滤镜`，选同一个 DLL，设为 **强制使用**。

> 滤镜注册为 `MERIT_DO_NOT_USE`，**不会被任何播放器自动插入** —— 这是故意的。
> 否则它会劫持机器上所有 DirectShow 图，包括你不希望被处理的那些。

播放器需要输出 **NV12**（LAV Video Decoder 默认就是），渲染器用 **EVR**。
滤镜同时接受 NV12 / RGB24 / RGB32。

---

## 使用

### 实时调参

界面是**通知区域（托盘）里的一个小图标** —— 和 LAV Filters 的做法一样，直接实现在
滤镜 DLL 内部：**不需要 Python，也不多开进程**。

播放视频时图标自动出现，停止播放自动消失：

| 操作 | 效果 |
| --- | --- |
| **右键图标** | 快速菜单：启用 / 风格 / 强度 / 局部色调 / 局部结构 / 打开控制面板 / 打开日志 / 关于 / 隐藏图标 |
| **双击图标** | 打开控制面板（带**滑块**，可连续调参，实时生效） |
| **播放器内** | 滤镜 → DLSSNR → 属性，打开同一个控制面板 |

控制面板行为：

- **单实例** —— 关掉只是隐藏，从托盘随时能找回**同一个**窗口
- **暂停不会让它消失** —— 显隐只看滤镜是否还在
- **参数实时生效** —— 拖动滑块立刻作用于下一帧，同时写回 `dlssnr_dshow.ini`
- **可设启动时自动显示** —— 面板中勾选 "启动时显示控制面板" 后，每个播放会话在引擎就绪时自动打开一次（与托盘右键菜单同一开关）

> 为什么滑块在窗口里而不是托盘菜单里：Windows 的托盘菜单本质是个 `HMENU`，
> 只能放文字/位图项，**无法容纳可拖动的控件**。madVR 的滑块同样在它的设置窗口里。

### 参数

`app\dlssnr_dshow.ini`：

| 键 | 说明 |
| --- | --- |
| `enabled` | 总开关。0 = 真正的直通（逐字节等于输入） |
| `style` | 风格 0-3（默认 / 自然 / 电影 / 风格3） |
| `intensity` | 强度 0-100（%） |
| `localtone` | 局部色调 0-100（%） |
| `localstruct` | 局部结构 0-100（%） |
| `autoshow` | 开始播放时是否自动打开控制面板（也可在控制面板或托盘右键菜单里切换） |

面板里改的会实时生效；拖动结束或切换选项时写回 `dlssnr_dshow.ini`，下次启动读取。

---

## 性能

实测（RTX 5070 Ti Laptop）：

| 分辨率 | 每帧耗时 | 折算帧率 |
| --- | --- | --- |
| 1920×1440（NV12 链路） | 14.3 ms | ~70 fps |
| 1920×1080 | ~10 ms | ~100 fps |

滤镜要额外做 `NV12 → BGR24 → 引擎 → BGR24 → NV12` 的往返转换，
1080p 下比 Python 直接调用多约 0.6–1.3 ms。

**建议上限 1080p60。** 1440p / 4K 时 GPU 会成为瓶颈（约 84% 占用），
1080p60 给解码和呈现只留约 7 ms 余量。

---

## 实现说明

`dlssnr_dshow.cpp` 是**完全自包含**的：Windows SDK 提供了 `strmbase.lib`
但**没有 `streams.h`**，也没有 ATL，所以 `CTransformFilter` 系列用不了。
本滤镜直接实现 `IPin` / `IMemInputPin` / `IBaseFilter`，
只复用系统自带的 `CLSID_MemoryAllocator`。

几个踩过的坑记录在源码注释里，包括：

- **注册必须设置 `REG_PINFLAG_B_OUTPUT`** —— 否则滤镜库里根本没有输出引脚，
  而 `IGraphBuilder` 不读这个标志（它直接问引脚），所以手工搭图测试会全绿、真播放器却全败
- **输出引脚不能缓存媒体类型** —— 播放器会用"断开重连输入引脚"来探测图，
  每次断开都会清掉缓存，播放器随后看到"0 种类型"就放弃，且**从不调用输出引脚的 Connect**
- **必须自建 MemoryAllocator** —— EVR 的分配器在 commit 前 `cbBuffer` 读作 0，`SetProperties` 会失败
- **绝不要伪造分辨率** —— 曾硬编码 1920×1080 兜底，导致图构建器用错误的几何连接

---

## 授权

自有代码采用 **MIT**（见 [`LICENSE`](LICENSE)）。

本仓库**不包含、也不得再分发**任何 NVIDIA 软件。
详见 [`docs/THIRD_PARTY.md`](docs/THIRD_PARTY.md)。

DLSS、RTX Video 是 NVIDIA Corporation 的商标与技术。本项目与 NVIDIA 无隶属关系，亦未获其背书。
