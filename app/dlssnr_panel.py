#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
dlssnr_panel.py -- DLSSNR live control panel for the DirectShow filter.

Copyright (c) 2026 Cyanke. MIT License — see LICENSE at the repository root.

This file is an original work: it is not derived from purkatyy/DLSS5- and has no
counterpart upstream. It talks to dlssnr_dshow.dll over a named shared-memory
block, so it works with any DirectShow player.

Answers two questions:
  1. "Is the filter actually loaded and working?"  -> the status area shows a
     live heartbeat, frame counters, engine state and a plain-language reason
     whenever frames are passing through untouched.
  2. "Can I tune DLSSNR while watching?"           -> the sliders write into a
     shared-memory block the filter reads once per frame. All four parameters
     were measured to take effect mid-session, so changes apply immediately
     with no restart and no interruption.

It talks to dlssnr_dshow.dll through a named shared memory block, so it works
whether the video is playing in PotPlayer or any other DirectShow player.

Usage:
    python dlssnr_panel.py            (or run with pythonw for no console)
"""
import ctypes
import ctypes.wintypes as wt
import sys
import tkinter as tk
from tkinter import ttk

SHARED_NAME = "Local\\DLSSNR_DShow_Shared_v1"
MAGIC = 0x4E534C44  # 'DLSN'

# Single-instance guard. The filter launches the panel automatically when its
# engine becomes ready, so a second copy would otherwise appear every time a
# player restarts. Holding a named mutex for the process lifetime makes the
# duplicate exit immediately.
SINGLETON_MUTEX = "Local\\DLSSNR_Panel_Singleton_v1"
_mutex_handle = None


def acquire_singleton():
    """True if we are the first instance. Keeps the handle alive on success."""
    global _mutex_handle
    try:
        k32 = ctypes.WinDLL("kernel32", use_last_error=True)
        k32.CreateMutexW.restype = wt.HANDLE
        k32.CreateMutexW.argtypes = [wt.LPVOID, wt.BOOL, wt.LPCWSTR]
        h = k32.CreateMutexW(None, False, SINGLETON_MUTEX)
        if not h:
            return True                     # cannot tell; better to show than not
        ERROR_ALREADY_EXISTS = 183
        if ctypes.get_last_error() == ERROR_ALREADY_EXISTS:
            return False
        _mutex_handle = h                   # hold it for the process lifetime
        return True
    except Exception:
        return True

def process_alive(pid):
    """True if a process with this pid is still running.

    This is how the panel tells "a filter is live" from "the player exited".
    The heartbeat alone CANNOT do it: this process holds its own mapping of the
    shared block, so after the player dies the mapping (and the last heartbeat
    value) survive and a `heartbeat > 0` test stays true forever -- which made
    the window pop back up after the player was closed.
    """
    if pid <= 0:
        return False
    PROCESS_QUERY_LIMITED_INFORMATION = 0x1000
    STILL_ACTIVE = 259
    ERROR_ACCESS_DENIED = 5
    try:
        k32 = ctypes.WinDLL("kernel32", use_last_error=True)
        k32.OpenProcess.restype = wt.HANDLE
        k32.OpenProcess.argtypes = [wt.DWORD, wt.BOOL, wt.DWORD]
        k32.GetExitCodeProcess.argtypes = [wt.HANDLE, ctypes.POINTER(wt.DWORD)]
        h = k32.OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, False, int(pid))
        if not h:
            # Access denied still proves the process exists (it is just not
            # ours to query), so treat only "not found" as dead.
            return ctypes.get_last_error() == ERROR_ACCESS_DENIED
        try:
            code = wt.DWORD()
            if not k32.GetExitCodeProcess(h, ctypes.byref(code)):
                return False
            return code.value == STILL_ACTIVE
        finally:
            k32.CloseHandle(h)
    except Exception:
        return False


STYLE_CHOICES = {"默认": 0, "自然": 1, "电影": 2, "风格3": 3}

REASON_TEXT = {
    0: ("正在处理", "green"),
    1: ("引擎加载中…(约 1-15 秒)", "orange"),
    2: ("已关闭(面板开关或 ini)", "gray"),
    3: ("输入不是 RGB24(播放器需输出 RGB24)", "red"),
    4: ("分辨率与引擎会话不一致(需重新 Pause)", "red"),
    5: ("引擎返回错误,已直通", "red"),
    6: ("引擎初始化失败(每进程只能一个尺寸,换分辨率需重启播放器)", "red"),
}


class SharedState(ctypes.Structure):
    _pack_ = 1
    _fields_ = [
        ("magic", wt.DWORD), ("version", wt.DWORD),
        ("structSize", wt.DWORD), ("pad0", wt.DWORD),
        ("heartbeat", ctypes.c_long), ("framesSeen", ctypes.c_long),
        ("framesProcessed", ctypes.c_long), ("framesPassthrough", ctypes.c_long),
        ("engineReady", ctypes.c_int), ("engineGaveUp", ctypes.c_int),
        ("engineW", ctypes.c_int), ("engineH", ctypes.c_int),
        ("videoW", ctypes.c_int), ("videoH", ctypes.c_int),
        ("inputBpp", ctypes.c_int), ("enabled", ctypes.c_int),
        ("lastProcessMs", ctypes.c_float), ("pid", ctypes.c_long),
        ("lastReason", ctypes.c_int), ("reqApplied", ctypes.c_int),
        ("status", ctypes.c_wchar * 128),
        ("reqSeq", ctypes.c_long), ("reqEnabled", ctypes.c_int),
        ("reqStyle", ctypes.c_int),
        ("reqIntensity", ctypes.c_float), ("reqLocalTone", ctypes.c_float),
        ("reqLocalStruct", ctypes.c_float), ("reqMix", ctypes.c_float),
    ]


class Channel:
    """Maps the shared block. create=False means attach-only (don't initialise)."""

    def __init__(self):
        self.k32 = ctypes.WinDLL("kernel32", use_last_error=True)
        self.k32.CreateFileMappingW.restype = wt.HANDLE
        self.k32.CreateFileMappingW.argtypes = [
            wt.HANDLE, wt.LPVOID, wt.DWORD, wt.DWORD, wt.DWORD, wt.LPCWSTR]
        self.k32.MapViewOfFile.restype = wt.LPVOID
        self.k32.MapViewOfFile.argtypes = [wt.HANDLE, wt.DWORD, wt.DWORD, wt.DWORD, ctypes.c_size_t]
        self.k32.UnmapViewOfFile.argtypes = [wt.LPVOID]
        self.k32.CloseHandle.argtypes = [wt.HANDLE]
        self.handle = None
        self.view = None
        self.state = None

    def open(self, create=True):
        PAGE_READWRITE = 0x04
        FILE_MAP_ALL_ACCESS = 0xF001F
        size = ctypes.sizeof(SharedState)
        self.handle = self.k32.CreateFileMappingW(
            wt.HANDLE(-1), None, PAGE_READWRITE, 0, size, SHARED_NAME)
        if not self.handle:
            return False
        # If the filter already created it, we attach to the existing block.
        self.view = self.k32.MapViewOfFile(self.handle, FILE_MAP_ALL_ACCESS, 0, 0, size)
        if not self.view:
            self.k32.CloseHandle(self.handle)
            self.handle = None
            return False
        self.state = ctypes.cast(self.view, ctypes.POINTER(SharedState)).contents
        if create and self.state.magic != MAGIC:
            ctypes.memset(self.view, 0, size)
            self.state.magic = MAGIC
            self.state.version = 1
            self.state.structSize = size
            self.state.reqEnabled = 1
            self.state.reqStyle = 0
            self.state.reqIntensity = 1.0
            self.state.reqLocalTone = 1.0
            self.state.reqLocalStruct = 1.0
        return True

    def close(self):
        if self.view:
            self.k32.UnmapViewOfFile(self.view)
            self.view = None
        if self.handle:
            self.k32.CloseHandle(self.handle)
            self.handle = None
        self.state = None


class Panel:
    def __init__(self, root):
        self.root = root
        self.ch = Channel()
        self.attached = self.ch.open(create=True)
        self.last_hb = -1
        self.stall_ticks = 0
        self.applied_seq_seen = -1
        self.shown = True          # window starts visible when launched manually
        # Which filter instance (player pid) the user dismissed. Closing the
        # window must stick for THAT filter, but a later player session should
        # still be able to surface the panel again -- so the dismissal is keyed
        # on the owning pid instead of a single sticky flag.
        self.closed_pid = None
        self.cur_pid = 0           # pid currently seen in the shared block

        root.title("DLSSNR 控制面板")
        root.resizable(False, False)

        pad = {"padx": 8, "pady": 4}

        # ---------------- status ----------------
        st = ttk.LabelFrame(root, text="滤镜状态")
        st.grid(row=0, column=0, sticky="ew", **pad)

        self.v_status = tk.StringVar(value="正在检测…")
        self.v_sub = tk.StringVar(value="")
        self.v_stats = tk.StringVar(value="")
        self.v_geom = tk.StringVar(value="")

        self.lbl_status = tk.Label(st, textvariable=self.v_status,
                                   font=("Microsoft YaHei UI", 12, "bold"),
                                   anchor="w", fg="gray")
        self.lbl_status.grid(row=0, column=0, sticky="w", padx=8, pady=(6, 0))
        ttk.Label(st, textvariable=self.v_sub, anchor="w",
                  foreground="#555").grid(row=1, column=0, sticky="w", padx=8)
        ttk.Label(st, textvariable=self.v_stats, anchor="w").grid(
            row=2, column=0, sticky="w", padx=8, pady=(4, 0))
        ttk.Label(st, textvariable=self.v_geom, anchor="w").grid(
            row=3, column=0, sticky="w", padx=8, pady=(0, 6))

        # ---------------- parameters ----------------
        pf = ttk.LabelFrame(root, text="DLSSNR 参数(实时生效)")
        pf.grid(row=1, column=0, sticky="ew", **pad)
        pf.columnconfigure(1, weight=1)

        self.v_enable = tk.BooleanVar(value=True)
        ttk.Checkbutton(pf, text="启用 DLSSNR(取消即直通,便于 A/B 对比)",
                        variable=self.v_enable,
                        command=self.push).grid(row=0, column=0, columnspan=3,
                                                sticky="w", padx=8, pady=(6, 2))

        ttk.Label(pf, text="风格").grid(row=1, column=0, sticky="w", padx=8)
        self.v_style = tk.StringVar(value="默认")
        cb = ttk.Combobox(pf, textvariable=self.v_style, width=10,
                          values=list(STYLE_CHOICES), state="readonly")
        cb.grid(row=1, column=1, sticky="w", pady=2)
        cb.bind("<<ComboboxSelected>>", lambda e: self.push())

        self.sl = {}
        rows = [("intensity", "强度", "overall effect strength"),
                ("localtone", "局部色调", "local tone mapping (contrast)"),
                ("localstruct", "局部结构", "local structure / detail")]
        r = 2
        for key, label, hint in rows:
            ttk.Label(pf, text=label).grid(row=r, column=0, sticky="w", padx=8)
            v = tk.IntVar(value=100)
            s = ttk.Scale(pf, from_=0, to=100, orient="horizontal",
                          variable=v, command=lambda _v, k=key: self.push())
            s.grid(row=r, column=1, sticky="ew", pady=2)
            num = ttk.Label(pf, width=5, anchor="e")
            num.grid(row=r, column=2, sticky="e", padx=(0, 8))
            self.sl[key] = (v, s, num, hint)
            r += 1

        self.v_hint = tk.StringVar(value="")
        ttk.Label(pf, textvariable=self.v_hint, foreground="#777",
                  wraplength=430, justify="left").grid(
            row=r, column=0, columnspan=3, sticky="w", padx=8, pady=(2, 8))

        # ---------------- buttons ----------------
        bf = ttk.Frame(root)
        bf.grid(row=2, column=0, sticky="ew", **pad)
        ttk.Button(bf, text="重置为默认", command=self.reset).grid(row=0, column=0, padx=(0, 6))
        ttk.Button(bf, text="重新检测", command=self.reattach).grid(row=0, column=1)
        ttk.Button(bf, text="打开日志", command=self.open_log).grid(row=0, column=2, padx=6)

        if not self.attached:
            self.v_status.set("无法创建共享内存")
            self.v_sub.set("面板无法与滤镜通信")

        self.sync_from_shared(initial=True)
        self.tick()

    # ------------------------------------------------------------------ push
    def push(self):
        """Slider moved: write the request block and bump the sequence number."""
        if not self.attached or not self.ch.state:
            return
        s = self.ch.state
        for key, (v, _s, num, _h) in self.sl.items():
            num.config(text="%d%%" % v.get())
        s.reqEnabled = 1 if self.v_enable.get() else 0
        s.reqStyle = STYLE_CHOICES.get(self.v_style.get(), 0)
        s.reqIntensity = self.sl["intensity"][0].get() / 100.0
        s.reqLocalTone = self.sl["localtone"][0].get() / 100.0
        s.reqLocalStruct = self.sl["localstruct"][0].get() / 100.0
        s.reqSeq = s.reqSeq + 1        # bump last: the filter keys off this

    def reset(self):
        self.v_enable.set(True)
        self.v_style.set("默认")
        for key, (v, _s, _n, _h) in self.sl.items():
            v.set(100)
        self.push()

    def reattach(self):
        self.ch.close()
        self.attached = self.ch.open(create=True)
        self.sync_from_shared(initial=True)

    def open_log(self):
        import os
        import subprocess
        # The filter writes its log (and its ini) next to the DLL, and this
        # script ships in that very same folder. Resolve it relative to
        # __file__ so the button keeps working when the repo is moved or the
        # two repositories are split apart -- a hardcoded absolute path here
        # silently broke as soon as the filter moved to dlssnr-filter\app\.
        here = os.path.dirname(os.path.abspath(__file__))
        p = os.path.join(here, "dlssnr_dshow.log")
        if os.path.exists(p):
            subprocess.Popen(["notepad.exe", p])
        else:
            self.v_sub.set("还没有日志文件(滤镜尚未被加载过)")

    # ------------------------------------------------------------------ read
    def sync_from_shared(self, initial=False):
        if not self.attached or not self.ch.state:
            return
        s = self.ch.state
        # Only adopt values the panel did not originate (e.g. before first push,
        # or after the filter was recreated with different ini settings).
        if initial and s.reqSeq != self.applied_seq_seen:
            self.v_enable.set(bool(s.reqEnabled))
            self.v_style.set(next((k for k, v in STYLE_CHOICES.items()
                                   if v == s.reqStyle), "默认"))
            self.sl["intensity"][0].set(int(round(s.reqIntensity * 100)))
            self.sl["localtone"][0].set(int(round(s.reqLocalTone * 100)))
            self.sl["localstruct"][0].set(int(round(s.reqLocalStruct * 100)))
            for key, (_v, _s, num, _h) in self.sl.items():
                num.config(text="%d%%" % _v.get())

    def tick(self):
        try:
            self.update_status()
        except Exception as e:                       # never let the panel die
            self.v_sub.set("面板错误: %s" % e)
        self.root.after(300, self.tick)

    def auto_show_hide(self, should_show):
        """Show the window once a filter is live; withdraw it when there is none.

        Hidden rather than destroyed, so sliders keep their positions and the
        panel reappears instantly the next time a player starts.

        Note the caller owns the "should it be visible?" decision (including the
        user's dismissal); this method only applies it, so it can never fight
        the close button.
        """
        if should_show == self.shown:
            return
        self.shown = should_show
        try:
            if should_show:
                self.root.deiconify()
                self.root.lift()
            else:
                self.root.withdraw()
        except Exception:
            pass

    def update_status(self):
        if not self.attached or not self.ch.state:
            self.v_status.set("未连接")
            self.v_sub.set("无法映射共享内存")
            self.auto_show_hide(False)
            return
        s = self.ch.state
        if s.magic != MAGIC:
            self.v_status.set("滤镜未加载")
            self.v_sub.set("共享内存存在但未被滤镜初始化 —— 请确认播放器已加载滤镜")
            self.v_stats.set("")
            self.v_geom.set("")
            self.auto_show_hide(False)
            return

        hb = int(s.heartbeat)
        if hb == self.last_hb:
            self.stall_ticks += 1
        else:
            self.stall_ticks = 0
            self.last_hb = hb

        alive = self.stall_ticks < 4          # ~1.2 s without a frame = paused

        # A different owning process means a NEW filter instance (the player was
        # restarted, or a second player opened). That is the event that clears a
        # previous dismissal, so the panel can surface itself again for the new
        # session.
        pid = int(s.pid)
        if pid != self.cur_pid:
            self.cur_pid = pid
            self.closed_pid = None
            self.last_hb = hb
            self.stall_ticks = 0

        # "The filter is live" needs BOTH a heartbeat and a live owning process.
        # The heartbeat alone is not enough: we hold our own mapping of the
        # shared block, so it outlives the player and keeps the last value, and
        # `hb > 0` would stay true after the player exited.
        filter_present = hb > 0 and process_alive(pid)

        # Auto show/hide. The window is visible while a filter is live, unless
        # the user dismissed THIS instance with the close button. Once the filter
        # goes away (player closed, or the ini disabled it) the panel hides
        # itself instead of lingering as a dead window. `alive` is deliberately
        # NOT used for visibility: pausing playback must not make the window
        # vanish mid-tune, and it must not resurrect a dismissed window either.
        should_show = filter_present and self.closed_pid != pid
        self.auto_show_hide(should_show)

        # main state line
        reason = int(s.lastReason)
        txt, colour = REASON_TEXT.get(reason, ("未知状态", "gray"))
        if not alive:
            self.v_status.set("已加载,但当前没有帧(暂停/未播放)")
            colour = "gray"
        else:
            self.v_status.set("已加载 · " + txt)
        self.lbl_status.config(fg=colour)

        self.v_sub.set("PID %d   %s" % (int(s.pid), s.status or ""))

        # counters + measured cost
        seen = int(s.framesSeen)
        proc = int(s.framesProcessed)
        pthru = int(s.framesPassthrough)
        pct = (100.0 * proc / seen) if seen else 0.0
        self.v_stats.set("帧: 收到 %d / 处理 %d / 直通 %d  (处理率 %.1f%%)   引擎 %s   %.2f ms/帧"
                         % (seen, proc, pthru, pct,
                            "就绪" if s.engineReady else ("失败" if s.engineGaveUp else "加载中"),
                            float(s.lastProcessMs)))

        bpp = int(s.inputBpp)
        self.v_geom.set("视频 %dx%d  像素格式 %d bpp%s   引擎会话 %dx%d"
                        % (int(s.videoW), int(s.videoH), bpp,
                           " (BGR24, 最优)" if bpp == 3 else " (非 RGB24, 无法处理)",
                           int(s.engineW), int(s.engineH)))

        # show whether the filter has actually consumed our latest request
        if int(s.reqApplied) != int(s.reqSeq):
            self.v_hint.set("已发送修改,等待滤镜在下一帧应用…")
        else:
            self.v_hint.set(
                "参数已生效。实测(平坦噪声场): 强度 50% 约去噪 4%, 100% 约 7.7%,"
                "0% 基本等于直通。\n"
                "取消勾选 = 真正的直通(逐字节等于输入); 强度拉到 0 仍会跑引擎。")


def main():
    # If the filter already started a panel, do not stack a second one.
    if not acquire_singleton():
        print("DLSSNR panel is already running.")
        return

    root = tk.Tk()
    try:
        root.call("tk", "scaling", 1.25)
    except Exception:
        pass

    auto = "--auto" in sys.argv      # launched by the filter
    p = Panel(root)
    if auto:
        # Start hidden and let the first status poll reveal it, so the window
        # does not flash before we know whether a filter is actually alive.
        p.shown = False
        try:
            root.withdraw()
        except Exception:
            pass

    def on_close():
        # Hide rather than exit: the panel stays ready for the next player run.
        # Remember WHICH filter instance was dismissed so the auto-show logic
        # does not immediately undo this, while a later player session can still
        # bring the window back (it has a new owning pid).
        p.closed_pid = p.cur_pid
        p.shown = False
        try:
            root.withdraw()
        except Exception:
            pass

    root.protocol("WM_DELETE_WINDOW", on_close)

    # A real quit path (tray would call this; keep it for Ctrl+C / taskkill).
    def on_quit():
        p.ch.close()
        root.destroy()
    root.bind_all("<Control-q>", lambda e: on_quit())

    root.mainloop()


if __name__ == "__main__":
    main()
