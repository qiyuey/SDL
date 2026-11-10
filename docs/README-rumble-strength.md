# Application strength and normalized SDL output

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
device encoding and output scheduling. Set
`SDL_JOYSTICK_HIDAPI_SWITCH2_EXPERIMENTAL_HD=1` to enable independent frequency
output on Switch 2 Pro. The amplitude and frequency mapping still requires
physical calibration. Controllers without frequency support can fall back to
conventional rumble with the peak amplitude of each band.
