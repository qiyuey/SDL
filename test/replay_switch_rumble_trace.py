"""Manually replay selected game-request envelopes on a wired Switch 2 Pro.

No gain, frequency alteration, onset boost, or invented hardware compensation.
This tests the current SDL output path; it cannot measure USB timing or force.
"""
import argparse
import ctypes as C
import os
from pathlib import Path
import time

from analyze_switch_rumble_trace import parse


def segments(rows):
    """Merge both sides by timestamp and return complete active episodes."""
    groups = {}
    for r in rows:
        groups.setdefault(r['ms'], []).append(r)
    states = {0: (0., 160., 0., 320.), 1: (0., 160., 0., 320.)}
    start, current, out = None, [], []
    for timestamp in sorted(groups):
        for r in groups[timestamp]:
            side = (r['handle'] >> 16) & 1
            states[side] = (r['low'], r['low_hz'], r['high'], r['high_hz'])
        values = states[0] + states[1]
        active = any(values[i] for i in (0, 2, 4, 6))
        if start is None and active:
            start = timestamp
        if start is not None:
            current.append((timestamp-start, values))
            if not active:
                out.append(current)
                start, current = None, []
    # An unfinished episode has no observed stop; do not guess its duration.
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('log', type=Path)
    ap.add_argument('--dll-dir', type=Path, default=Path(__file__).resolve().parents[2] / 'SavageWindRecomp/build')
    args = ap.parse_args()
    episodes = segments(parse(args.log.read_text(encoding='utf-8', errors='replace')))
    if not episodes:
        raise ValueError('No complete vibration episodes in this log.')
    # Compare a short and a long actual effect, not arbitrary test tones.
    choices = [min(episodes, key=lambda e: e[-1][0]), max(episodes, key=lambda e: e[-1][0])]
    print('游戏原始震动包络回放 / USB Switch 2 Pro\n不放大、不修改频率；专利频率映射仍待标定。')
    for i, e in enumerate(choices, 1):
        peak = max(v[j] for _, v in e for j in (0, 2, 4, 6))
        print(f'{i}: 实际请求时长 {e[-1][0]} ms，峰值 {peak:.5f}，{len(e)} 个变化点')
    print('初始化中；请关闭游戏及其他手柄测试程序。')
    cookie = os.add_dll_directory(str(args.dll_dir.resolve()))
    usb = C.CDLL(str((args.dll_dir / 'libusb-1.0.dll').resolve()))
    s = C.CDLL(str((args.dll_dir / 'SDL3.dll').resolve()))

    def bind(name, result, arguments):
        f = getattr(s, name)
        f.restype, f.argtypes = result, arguments
        return f

    init = bind('SDL_Init', C.c_bool, [C.c_uint32])
    hint = bind('SDL_SetHint', C.c_bool, [C.c_char_p, C.c_char_p])
    update = bind('SDL_UpdateGamepads', None, [])
    get = bind('SDL_GetGamepads', C.POINTER(C.c_uint32), [C.POINTER(C.c_int)])
    vendor = bind('SDL_GetGamepadVendorForID', C.c_uint16, [C.c_uint32])
    product = bind('SDL_GetGamepadProductForID', C.c_uint16, [C.c_uint32])
    op = bind('SDL_OpenGamepad', C.c_void_p, [C.c_uint32])
    send = bind('SDL_RumbleGamepadFrequenciesEx', C.c_bool, [C.c_void_p]+[C.c_float]*8+[C.c_uint32])
    stop = bind('SDL_RumbleGamepad', C.c_bool, [C.c_void_p, C.c_uint16, C.c_uint16, C.c_uint32])
    error = bind('SDL_GetError', C.c_char_p, [])
    close = bind('SDL_CloseGamepad', None, [C.c_void_p])
    shutdown = bind('SDL_Quit', None, [])
    free = bind('SDL_free', None, [C.c_void_p])
    hint(b'SDL_JOYSTICK_HIDAPI_SWITCH2_EXPERIMENTAL_HD', b'1')
    hint(b'SDL_JOYSTICK_HIDAPI_SWITCH2_HD_GAIN', b'1')
    hint(b'SDL_HIDAPI_IGNORE_DEVICES', b'0x057e/0x2009')
    pad = None
    try:
        if not init(0x2000):
            raise RuntimeError(error().decode(errors='replace'))
        count = C.c_int()
        ids = get(C.byref(count))
        try:
            for i in range(count.value):
                if vendor(ids[i]) == 0x057e and product(ids[i]) == 0x2069:
                    pad = op(ids[i])
                    break
        finally:
            free(ids)
        if not pad:
            raise RuntimeError('未能打开有线手柄：'+error().decode(errors='replace'))
        while True:
            answer = input('\n输入1或2并回车播放一段原始效果；q退出：').strip()
            if answer.lower() == 'q':
                break
            if answer not in ('1', '2'):
                continue
            episode = choices[int(answer)-1]
            print('播放中…')
            begin = time.perf_counter()
            lateness = []
            for ms, values in episode:
                deadline = begin + ms/1000
                while time.perf_counter() < deadline:
                    update()
                    time.sleep(.001)
                lateness.append(max(0., (time.perf_counter()-deadline)*1000))
                if not send(pad, *values, 100):
                    raise RuntimeError(error().decode(errors='replace'))
                update()
            stop(pad, 0, 0, 0)
            for _ in range(20):
                update()
                time.sleep(.005)
            print(f'已停止；API调度最大迟到 {max(lateness):.2f} ms（不是USB延迟）。')
    finally:
        if pad:
            stop(pad, 0, 0, 0)
            for _ in range(20):
                update()
                time.sleep(.005)
            close(pad)
        shutdown()
        cookie.close()


if __name__ == '__main__':
    try:
        main()
    except KeyboardInterrupt:
        print('\n已取消。')
