"""Interactive local USB haptics lab. No output before Start; no microphone use.

Uses the workspace's custom SDL3, not a stock PySDL installation.
Run with a Python installation that includes tkinter.
"""
import ctypes as C
import math
import queue
import threading
import time
from pathlib import Path
import os
import tkinter as tk
from tkinter import ttk

ROOT = Path(__file__).resolve().parents[1]
DLL_DIR = ROOT.parent / 'SavageWindRecomp' / 'build'
def enable_hidpi():
    # Must run before Tk creates any HWND; fall back on older Windows.
    try:
        C.windll.user32.SetProcessDpiAwarenessContext.argtypes = [C.c_void_p]
        if C.windll.user32.SetProcessDpiAwarenessContext(C.c_void_p(-4)):
            return
    except (AttributeError, OSError):
        pass
    try:
        C.windll.shcore.SetProcessDpiAwareness(2)
    except (AttributeError, OSError):
        C.windll.user32.SetProcessDPIAware()


def window_dpi(root):
    try:
        get = C.windll.user32.GetDpiForWindow
        get.argtypes, get.restype = [C.c_void_p], C.c_uint
        return get(root.winfo_id()) or 96
    except (AttributeError, OSError):
        return round(root.winfo_fpixels('1i'))


BG, PANEL, INK, ACCENT = '#11141e', '#202535', '#edf0fa', '#b49aff'

# Traditional public-domain melody, C major. C4-A4 stays within the pad's
# 40-480 Hz range. These are requested pitches, not calibrated measurements.
TWINKLE_PITCHES = {'C': 261.626, 'D': 293.665, 'E': 329.628,
                  'F': 349.228, 'G': 391.995, 'A': 440.0}
TWINKLE_PHRASES = ('CCGGAAG', 'FFEEDDC', 'GGFFEED',
                   'GGFFEED', 'CCGGAAG', 'FFEEDDC')
TWINKLE_BEAT = .4
TWINKLE = tuple((note, 2 if i == 6 else 1)
                for phrase in TWINKLE_PHRASES for i, note in enumerate(phrase))
TWINKLE_DURATION = sum(beats for _, beats in TWINKLE) * TWINKLE_BEAT


def twinkle_at(elapsed):
    """Return (Hz, sounding, note index); silence separates repeated notes."""
    if elapsed < 0:
        return TWINKLE_PITCHES['C'], False, 0
    remaining = elapsed
    for index, (note, beats) in enumerate(TWINKLE):
        duration = beats * TWINKLE_BEAT
        if remaining < duration:
            return TWINKLE_PITCHES[note], remaining < duration - .08, index
        remaining -= duration
    return TWINKLE_PITCHES['C'], False, len(TWINKLE)


def rumble_values(hz, amp, side, component, high_hz=320., high_amp=.25, audition='both'):
    """Map independent components to the eight SDL values without merging sides."""
    if component == 'dual':
        low = amp if audition != 'high' else 0.
        high = high_amp if audition != 'low' else 0.
    else:
        low = amp if component in ('second', 'mix') else 0.
        high = amp if component in ('first', 'mix') else 0.
        high_hz = hz
        if component == 'mix':
            low *= .5
            high *= .5
    left, right = side != 'right', side != 'left'
    return (low*left, hz, high*left, high_hz, low*right, hz, high*right, high_hz)


class Output:
    def __init__(self):
        self.lock = threading.Lock()
        self.target = (False, 160., .25, 'both', 'second', 320., .25, 'both')
        self.quit = threading.Event()
        self.messages = queue.Queue()
        self.thread = threading.Thread(target=self.run, daemon=True)
        self.thread.start()

    def set(self, active, hz, amp, side, component, high_hz=320., high_amp=.25, audition='both'):
        with self.lock:
            self.target = (active, hz, amp, side, component, high_hz, high_amp, audition)
            self.updated = time.monotonic()

    def run(self):
        s, pad, cookie = None, None, None
        try:
            cookie = os.add_dll_directory(str(DLL_DIR))
            usb = C.CDLL(str(DLL_DIR / 'libusb-1.0.dll'))
            s = C.CDLL(str(DLL_DIR / 'SDL3.dll'))

            def bind(name, restype, args):
                f = getattr(s, name)
                f.restype, f.argtypes = restype, args
                return f

            error = bind('SDL_GetError', C.c_char_p, [])
            hint = bind('SDL_SetHint', C.c_bool, [C.c_char_p, C.c_char_p])
            hint(b'SDL_JOYSTICK_HIDAPI_SWITCH2_EXPERIMENTAL_HD', b'1')
            hint(b'SDL_HIDAPI_IGNORE_DEVICES', b'0x057e/0x2009')
            init = bind('SDL_Init', C.c_bool, [C.c_uint32])
            get = bind('SDL_GetGamepads', C.POINTER(C.c_uint32), [C.POINTER(C.c_int)])
            vendor = bind('SDL_GetGamepadVendorForID', C.c_uint16, [C.c_uint32])
            product = bind('SDL_GetGamepadProductForID', C.c_uint16, [C.c_uint32])
            op = bind('SDL_OpenGamepad', C.c_void_p, [C.c_uint32])
            close = bind('SDL_CloseGamepad', None, [C.c_void_p])
            connected = bind('SDL_GamepadConnected', C.c_bool, [C.c_void_p])
            free = bind('SDL_free', None, [C.c_void_p])
            update = bind('SDL_UpdateGamepads', None, [])
            stop = bind('SDL_RumbleGamepad', C.c_bool, [C.c_void_p, C.c_uint16, C.c_uint16, C.c_uint32])
            send = bind('SDL_RumbleGamepadFrequenciesEx', C.c_bool,
                        [C.c_void_p] + [C.c_float] * 8 + [C.c_uint32])
            shutdown = bind('SDL_Quit', None, [])
            if not init(0x2000):
                raise RuntimeError(error().decode(errors='replace'))
            self.messages.put(('status', '正在查找有线 Switch 2 Pro…'))
            last_send, last_scan, active_before = 0., 0., False
            while not self.quit.is_set():
                update()
                now = time.monotonic()
                if pad and not connected(pad):
                    close(pad)
                    pad = None
                    active_before = False
                    self.messages.put(('lost', '连接已断开，已取消播放；重新连接后需手动开始。'))
                if not pad and now - last_scan >= 1:
                    last_scan = now
                    n = C.c_int()
                    ids = get(C.byref(n))
                    try:
                        for i in range(n.value):
                            if vendor(ids[i]) == 0x057e and product(ids[i]) == 0x2069:
                                pad = op(ids[i])
                                if pad:
                                    self.set(False, 160., .25, 'both', 'second')
                                    self.messages.put(('ready', '有线 Switch 2 Pro 已连接 · 尚未播放'))
                                else:
                                    self.messages.put(('lost', '打开失败：' + error().decode(errors='replace')))
                                break
                    finally:
                        free(ids)
                with self.lock:
                    active, hz, amp, side, component, high_hz, high_amp, audition = self.target
                    fresh = now - getattr(self, 'updated', 0) < .3
                active = active and fresh and bool(pad)
                if pad and active and now - last_send >= .04:
                    values = rumble_values(hz, amp, side, component, high_hz, high_amp, audition)
                    ok = send(pad, *values, 120)
                    if not ok:
                        self.set(False, hz, amp, side, component)
                        stop(pad, 0, 0, 0)
                        self.messages.put(('lost', '输出失败：' + error().decode(errors='replace')))
                    last_send = now
                elif pad and active_before and not active:
                    stop(pad, 0, 0, 0)
                active_before = active
                self.quit.wait(.005)
        except Exception as exc:
            self.messages.put(('error', str(exc)))
        finally:
            if s:
                try:
                    if pad:
                        stop(pad, 0, 0, 0)
                        for _ in range(20):
                            update()
                            time.sleep(.005)
                        close(pad)
                    shutdown()
                except Exception:
                    pass
            if cookie:
                cookie.close()


class Lab:
    def __init__(self, root):
        self.root, self.active, self.ready = root, False, False
        self.hz, self.amp = tk.DoubleVar(value=160), tk.DoubleVar(value=.25)
        self.side, self.component = tk.StringVar(value='both'), tk.StringVar(value='second')
        self.high_hz, self.high_amp = tk.DoubleVar(value=320), tk.DoubleVar(value=.25)
        self.audition = tk.StringVar(value='both')
        self.mode = 'free'
        self.started = 0.
        root.title('HD2 震动实验室 · Switch 2 Pro USB')
        root.update_idletasks()
        self.dpi = window_dpi(root)
        self.scale = self.dpi / 96.0
        root.tk.call('tk', 'scaling', self.dpi / 72.0)
        root.geometry(f'{round(1040*self.scale)}x{round(900*self.scale)}')
        root.minsize(round(850*self.scale), round(810*self.scale))
        root.configure(bg=BG)
        style = ttk.Style()
        style.theme_use('clam')
        style.configure('TFrame', background=BG)
        style.configure('TLabel', background=BG, foreground=INK, font=('Microsoft YaHei UI', 11))
        style.configure('TButton', font=('Microsoft YaHei UI', 11), padding=9)
        style.configure('TRadiobutton', background=BG, foreground=INK, font=('Microsoft YaHei UI', 10))
        body = ttk.Frame(root, padding=24)
        body.pack(fill='both', expand=True)
        ttk.Label(body, text='HD2 震动实验室', font=('Microsoft YaHei UI', 24, 'bold')).pack(anchor='w')
        ttk.Label(body, text='自由调节频率与强度 · 固定频率对比 · 40–480 Hz 扫频').pack(anchor='w', pady=(3, 10))
        self.status = tk.StringVar(value='初始化 SDL3 / USB 驱动中，请稍候…（不会自动震动）')
        ttk.Label(body, textvariable=self.status, foreground=ACCENT).pack(anchor='w')
        self.wave = tk.Canvas(body, height=100, bg=PANEL, highlightthickness=0)
        self.wave.pack(fill='x', pady=12)
        presets = ttk.Frame(body)
        presets.pack(fill='x')
        for name, hz in [('50 Hz', 50), ('200 Hz', 200), ('400 Hz', 400)]:
            ttk.Button(presets, text=name, command=lambda f=hz: self.preset(f)).pack(side='left', padx=(0, 8))
        ttk.Button(presets, text='自由模式', command=lambda: self.preset(self.hz.get())).pack(side='left', padx=8)
        ttk.Button(presets, text='40 → 480 Hz 扫频', command=self.sweep).pack(side='left', padx=8)
        ttk.Button(presets, text='♪ 小星星', command=self.twinkle).pack(side='left', padx=8)
        self.readout = tk.StringVar()
        ttk.Label(body, textvariable=self.readout, font=('Microsoft YaHei UI', 18, 'bold')).pack(anchor='w', pady=12)
        self.pad = tk.Canvas(body, height=230, bg=PANEL, highlightthickness=1, highlightbackground='#59617a', cursor='crosshair')
        self.pad.pack(fill='both', expand=True)
        self.pad.bind('<Button-1>', self.drag)
        self.pad.bind('<B1-Motion>', self.drag)
        ttk.Label(body, text='横轴：40–480 Hz　纵轴：0–100% 强度　拖动只调参；开始后实时生效').pack(anchor='w', pady=5)
        row = ttk.Frame(body)
        row.pack(fill='x', pady=4)
        for text, val in [('左侧', 'left'), ('右侧', 'right'), ('双侧', 'both')]:
            ttk.Radiobutton(row, text=text, variable=self.side, value=val).pack(side='left', padx=8)
        for text, val in [('第二分量', 'second'), ('第一分量', 'first'), ('同频各半', 'mix')]:
            ttk.Radiobutton(row, text=text, variable=self.component, value=val,
                            command=lambda: self.preset(self.hz.get())).pack(side='left', padx=8)
        dual = ttk.Frame(body)
        dual.pack(fill='x', pady=4)
        ttk.Button(dual, text='双频叠加 160 + 320 Hz', command=self.dual).pack(side='left')
        for text, val in [('仅低频', 'low'), ('仅高频', 'high'), ('双频叠加', 'both')]:
            ttk.Radiobutton(dual, text=text, variable=self.audition, value=val,
                            command=self.select_dual).pack(side='left', padx=8)
        highrow = ttk.Frame(body)
        highrow.pack(fill='x', pady=4)
        self.high_readout = tk.StringVar()
        ttk.Label(highrow, textvariable=self.high_readout, width=34).pack(side='left')
        ttk.Scale(highrow, from_=40, to=480, variable=self.high_hz).pack(side='left', fill='x', expand=True, padx=8)
        ttk.Scale(highrow, from_=0, to=1, variable=self.high_amp).pack(side='left', fill='x', expand=True, padx=8)
        ttk.Label(body, text='双频模式：二维面板调低频；上方两滑条调高频 Hz / 强度。试听切换保持各分量原强度。').pack(anchor='w')
        row = ttk.Frame(body)
        row.pack(fill='x', pady=8)
        self.start_button = ttk.Button(row, text='▶ 开始（最多 10 秒）', command=self.start, state='disabled')
        self.start_button.pack(side='left')
        ttk.Button(row, text='■ 立即停止 / Esc', command=self.stop).pack(side='left', padx=12)
        ttk.Label(body, text='频率为专利公式候选值，尚未实测标定。波形仅为示意，不是手柄测量结果。', foreground='#b2b8ca').pack(anchor='w')
        ttk.Label(body, text='输出使用完整 10 位振幅、增益 1。切出窗口、断开或超时会取消播放。', foreground='#b2b8ca').pack(anchor='w')
        root.bind('<Escape>', lambda e: self.stop())
        root.bind('<FocusOut>', lambda e: root.after(80, self.focus_check))
        root.protocol('WM_DELETE_WINDOW', self.close)
        self.pack_metrics = []
        def remember(widget):
            for child in widget.winfo_children():
                if child.winfo_manager() == 'pack':
                    info = child.pack_info()
                    values = {}
                    for key in ('padx', 'pady', 'ipadx', 'ipady'):
                        value = info[key]
                        parts = (value,) if isinstance(value, (int, float)) else root.tk.splitlist(value)
                        values[key] = tuple(int(v) for v in parts)
                    self.pack_metrics.append((child, values))
                remember(child)
        remember(body)
        self.body, self.style = body, style
        self.resize_metrics()
        root.bind('<Configure>', self.dpi_changed, add='+')
        self.pending_drag = None
        self.grid_lines = [self.pad.create_line(0, 0, 0, 0, fill='#42495f') for _ in range(10)]
        self.cursor_item = self.pad.create_oval(0, 0, 0, 0, fill=ACCENT, outline=INK)
        self.wave_item = self.wave.create_line(0, 0, 1, 0, fill=ACCENT, smooth=False)
        self.grid_key = self.wave_key = self.readout_key = None
        self.output = Output()
        self.tick()

    def px(self, value):
        return max(1, round(value * self.scale))

    def resize_metrics(self):
        self.body.configure(padding=self.px(24))
        self.style.configure('TButton', padding=self.px(9))
        self.wave.configure(height=self.px(100))
        self.pad.configure(height=self.px(230), highlightthickness=self.px(1))
        for child, metrics in self.pack_metrics:
            child.pack_configure(**{key: tuple(round(v*self.scale) for v in value)
                                    for key, value in metrics.items()})

    def dpi_changed(self, event):
        if event.widget != self.root:
            return
        dpi = window_dpi(self.root)
        if dpi != self.dpi:
            self.dpi, self.scale = dpi, dpi/96.0
            self.root.tk.call('tk', 'scaling', dpi/72.0)
            # Refresh named widget styles as Tk 8.6 may retain font metrics.
            for name, size in [('TLabel', 11), ('TButton', 11), ('TRadiobutton', 10)]:
                self.style.configure(name, font=('Microsoft YaHei UI', size))
            self.root.minsize(self.px(850), self.px(810))
            self.resize_metrics()

    def dual(self):
        self.stop()
        self.pending_drag = None
        self.mode = 'dual'
        self.hz.set(160)
        self.high_hz.set(320)
        self.amp.set(.25)
        self.high_amp.set(.25)
        self.audition.set('both')
        self.start_button.configure(text='▶ 开始双频（最多 10 秒）')
        self.status.set('双频已准备：低160 Hz / 高320 Hz，各25%。点击开始；可切换单分量作对照。')

    def select_dual(self):
        if self.mode != 'dual':
            self.stop()
            self.pending_drag = None
            self.mode = 'dual'
        self.start_button.configure(text='▶ 开始双频（最多 10 秒）')

    def preset(self, hz):
        self.pending_drag = None
        self.stop()
        self.mode = 'free'
        self.hz.set(hz)
        self.start_button.configure(text='▶ 开始（最多 10 秒）')

    def sweep(self):
        self.pending_drag = None
        self.stop()
        self.mode = 'sweep'
        self.hz.set(40)
        self.start_button.configure(text='▶ 开始扫频（8 秒）')
        self.status.set('扫频已准备：点击开始，8 秒内从 40 到 480 Hz；不会自动播放。')

    def twinkle(self):
        self.pending_drag = None
        self.stop()
        self.mode = 'twinkle'
        self.hz.set(TWINKLE_PITCHES['C'])
        self.amp.set(1.0)
        self.component.set('second')
        self.start_button.configure(text=f'▶ 播放小星星（{TWINKLE_DURATION:.1f} 秒）')
        self.status.set('小星星已准备：强度100%，第二分量；点击播放，音符间自动停顿，不使用扬声器。')

    def drag(self, event):
        # Mouse events can arrive faster than paint. Consume only the latest one.
        self.pending_drag = (event.x, event.y)

    def apply_drag(self):
        if self.pending_drag is None:
            return
        x, y = self.pending_drag
        self.pending_drag = None
        if self.mode == 'twinkle':
            self.stop()
        if self.mode != 'dual':
            self.start_button.configure(text='▶ 开始（最多 10 秒）')
            self.mode = 'free'
        w, h = self.pad.winfo_width(), self.pad.winfo_height()
        margin = self.px(20)
        self.hz.set(40 + 440 * min(1, max(0, (x - margin) / max(1, w - 2*margin))))
        self.amp.set(min(1, max(0, 1 - (y - margin) / max(1, h - 2*margin))))

    def start(self):
        if self.ready:
            self.active, self.started = True, time.monotonic()

    def stop(self):
        self.active = False
        if hasattr(self, 'output'):
            self.output.set(False, self.hz.get(), self.amp.get(), self.side.get(), self.component.get())

    def focus_check(self):
        if self.root.focus_displayof() is None:
            self.stop()

    def close(self):
        self.stop()
        self.output.quit.set()
        self.root.withdraw()
        def finish():
            if self.output.thread.is_alive():
                self.root.after(50, finish)
            else:
                self.root.destroy()
        finish()

    def tick(self):
        frame_start = time.monotonic()
        self.apply_drag()
        while not self.output.messages.empty():
            kind, message = self.output.messages.get_nowait()
            self.status.set(message)
            if kind == 'ready':
                self.stop()
                self.ready = True
            elif kind in ('lost', 'error'):
                self.stop()
                self.ready = False
            self.start_button.configure(state='normal' if self.ready else 'disabled')
        elapsed = time.monotonic() - self.started
        sounding, note_index = True, None
        if self.active:
            limit = TWINKLE_DURATION if self.mode == 'twinkle' else 8 if self.mode == 'sweep' else 10
            if elapsed >= limit:
                self.stop()
                self.status.set('本轮已结束，已停止。可调整参数后再次开始。')
            elif self.mode == 'sweep':
                self.hz.set(40 + 440 * elapsed / 8)
            elif self.mode == 'twinkle':
                frequency, sounding, note_index = twinkle_at(elapsed)
                self.hz.set(frequency)
        hz, amp = self.hz.get(), self.amp.get()
        dual_mode = self.mode == 'dual'
        component = 'dual' if dual_mode else self.component.get()
        hh, ha = self.high_hz.get(), self.high_amp.get()
        self.output.set(self.active and sounding, hz, amp, self.side.get(), component, hh, ha, self.audition.get())
        high_text = f'高频 {hh:.1f} Hz / 强度 {ha*100:.0f}%'
        if self.high_readout.get() != high_text:
            self.high_readout.set(high_text)
        readout = f'{hz:6.1f} Hz    强度 {amp * 100:5.1f}%    ' + ('● 正在播放' if self.active else '○ 已停止')
        if dual_mode:
            readout = f'低 {hz:.1f} Hz / {amp*100:.0f}%  +  高 {hh:.1f} Hz / {ha*100:.0f}%' + ('  ● 播放' if self.active else '  ○ 停止')
        if note_index is not None:
            readout += f'  ♪ {TWINKLE[note_index][0]}  {note_index+1}/{len(TWINKLE)}'
        if readout != self.readout_key:
            self.readout.set(readout)
            self.readout_key = readout
        w, h = self.pad.winfo_width(), self.pad.winfo_height()
        m, radius = self.px(20), self.px(11)
        grid_key = (w, h, self.scale)
        if grid_key != self.grid_key:
            for i in range(5):
                x = m + (w - 2*m) * i / 4
                y = m + (h - 2*m) * i / 4
                self.pad.coords(self.grid_lines[i*2], x, m, x, h-m)
                self.pad.coords(self.grid_lines[i*2+1], m, y, w-m, y)
            for item in self.grid_lines:
                self.pad.itemconfigure(item, width=self.px(1))
            self.pad.itemconfigure(self.cursor_item, width=self.px(2))
            self.wave.itemconfigure(self.wave_item, width=self.px(2))
            self.grid_key = grid_key
        x, y = m + (w - 2*m) * (hz - 40) / 440, m + (h - 2*m) * (1 - amp)
        self.pad.coords(self.cursor_item, x-radius, y-radius, x+radius, y+radius)
        ww, wh = max(2, self.wave.winfo_width()), self.wave.winfo_height()
        wave_key = (ww, wh, hz, amp, self.active, dual_mode, hh, ha, self.audition.get())
        if self.active or wave_key != self.wave_key:
            # Fixed sample budget avoids more work just because the monitor is HiDPI.
            count = 256
            phase = frame_start * 3 if self.active else 0
            points = []
            for i in range(count + 1):
                fraction = i / count
                value = amp * math.sin(fraction * math.tau * hz/40 - phase)
                if dual_mode:
                    if self.audition.get() == 'high':
                        value = 0.
                    if self.audition.get() != 'low':
                        value += ha * math.sin(fraction * math.tau * hh/40 - phase)
                    value *= .5  # fixed display scale only; hardware amplitudes are unchanged
                points.extend((fraction * ww, wh/2 + wh*.36 * value))
            self.wave.coords(self.wave_item, *points)
            self.wave_key = wave_key
        elapsed_ms = (time.monotonic() - frame_start) * 1000
        self.root.after(max(1, round(16 - elapsed_ms)), self.tick)



if __name__ == '__main__':
    enable_hidpi()
    root = tk.Tk()
    Lab(root)
    root.mainloop()
