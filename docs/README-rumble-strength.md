# Application strength and normalized SDL output

Policy updated 2026-11-06 at the workspace owner's request. This is a playback
preference, not a measured transfer function or a native HD Rumble 2 specification.

The application owns strength. For source amplitudes `a_low`, `a_high`, a strength
percentage `p` in 0..100 and a source base gain `b`, compute:

```text
gain = b * p / 100
scale = gain / max(1, max(a_low, a_high) * gain)
output_low = a_low * scale
output_high = a_high * scale
```

Use one shared scale per side; do not alter frequency or timing. Switch content
uses `b=5` (100%=5x, 20%=original); Switch 2 content uses `b=1`
(100%=original). Clamp the user percentage and validate source samples before
scaling. Do not overwrite original game state or scale it again when refreshing.

`SDL_RumbleGamepadFrequenciesEx` takes these final normalized amplitudes in
0..1 plus frequencies in Hz. Its public signature remains unchanged. SDL does
device encoding and output scheduling; it neither detects source game generation
nor adds application strength. The former
`SDL_JOYSTICK_HIDAPI_SWITCH2_HD_GAIN` hint is no longer read. Replacing an older
DLL is required for this behavior; source edits do not change deployed binaries.
The Switch 2 Pro experimental frequency path still requires its existing enable
hint and still needs physical calibration. Non-frequency-capable devices may
fall back to conventional output, losing frequency and side detail.

Workspace callers:

- Lichii selects the source base gain with `CurrentProgramIsSwitch2`. Its SDL
  fallback is linear and does not add the former exponential boost.
- Savage Wind Recomp uses 5x at 100%, with a 0..100 strength setting. Native
  and Python playback use the same scale.
- TOTK Recomp uses 5x at 100%, defaults to 50%, and no longer applies its
  previous amplitude curve. The old curve key is accepted but ignored.
- SMO Recomp retains the existing 0..1 config representation for strength:
  1 means 100%=5x and 0.2 is original. The SDL gamepad path preserves both
  sides' two amplitude/frequency components; its plain-joystick fallback is
  conventional. It requires the custom SDL3 headers/library/DLL and libusb.
- Python catalog playback selects 5x for BOTW and 1x for Ocarina. The HD2 lab
  and codec/API tests submit direct normalized test values, without a game gain.

Native test expectations were updated for this policy. No C/C++ build or
physical-controller test was performed for this change, at the owner's request.
Native HD Rumble 2 mode-2 behavior still awaits a Switch 2 HID system module.
