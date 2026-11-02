# Local Betop KP70C companion haptics

## Maintained capability contract

SDL owns device discovery, physical pairing, protocol encoding, validation,
worker serialization, finite leases, periodic refresh and neutral shutdown.
Game actions, content profiles, actor filters and priorities belong to each
game repository. No game name, game offset, bow state or AI recognition model
is required by the SDL backend. The local API is KP70C-specific; it does not
claim a portable adaptive-trigger contract across unrelated manufacturers.

| Capability | Interface / mode | Physical evidence on Windows receiver 20bc:5174 |
| --- | --- | --- |
| Separate grip envelopes | `SDL_RumbleGamepadFrequenciesEx` | Perceived output; submitted Hz cannot be retained by this report |
| Trigger resistance | `SDL_BETOP_TRIGGER_RESISTANCE` | Both sides and normal stop confirmed |
| Trigger vibration | `SDL_BETOP_TRIGGER_VIBRATION` | Both sides clear while pressed, normal stop confirmed |
| Trigger breakthrough | `SDL_BETOP_TRIGGER_BREAKTHROUGH` | Both sides clear and responsive with prearming/refresh, normal stop confirmed |
| Shoulder direct drive | `SDL_BETOP_SHOULDER_DIRECT`, `SDL_BETOP_SHOULDER_DIRECT_ON` | Both sides clear, normal stop confirmed |
| Trigger follow | `SDL_BETOP_TRIGGER_FOLLOW` | Encoding only; no positive physical validation |
| Shoulder crisp/soft/heavy | Legacy preset selectors | No perceived output in the tested preset sequence |

The named enums preserve the existing byte-sized structure layout. The
`SDL.gamepad.betop.haptics` property means a companion was opened, not that
every mode or transport has passed physical testing. Wired mode remains
unverified on hardware. Callers submit the complete trigger/shoulder state;
all-zero clears those actuators, while grip rumble must be stopped separately.
Active leases are 1-1000 ms; a 100 ms lease renewed at 25-50 ms is appropriate
for an application that can maintain that cadence. Device refresh is internal
and does not extend a lease without an application submission.

Build the Release diagnostic `testbetophaptics`, place SDL3.dll beside it or on
PATH, and use `--list` for enumeration. Physical modes include `--trigger-test`,
`--trigger-vibration-left/right`, `--trigger-breakthrough-left/right`, and
`--shoulder-direct-left/right`; each side suffix is a separate option. Physical
tests require deliberate execution and are not run by the ordinary build.
Local interactive wrappers and timestamped session logs are not versioned.

The sections below retain the investigation and validation history. The
capability table above is the current summary; unsuccessful early experiments
must not be confused with later successful direct-drive or refreshed tests.

This local extension pairs the Windows input gamepad with its vendor HID
interface. Only VID `20bc`, PID `5173` or `5174`, and the observed vendor usage
pages `00ff`, `ff00`, `ff80` / usages 1 or 3 are eligible.

Native input paths must match a Windows ContainerId. A synthetic `XInput#N`
path must identify the only connected XInput slot with that exact VID/PID and
the only corresponding vendor HID interface; ambiguous matches are rejected.
KP70C is excluded from GameInput and retained by the XInput backend even when
GameInput handles other controllers. The ordinary gamepad input path remains
separate from the companion's effects.

`SDL_RumbleGamepadFrequenciesEx` uses left/right amplitude envelopes on this
device. The protocol cannot preserve the submitted Hz values, so this is not
native Switch HD rumble. `SDL_SetGamepadBetopEffects` submits independent
trigger and shoulder effects with a 1–1000 ms lease. A worker serializes output
and expires it without event pumping. Closing the gamepad attempts neutral
reports and joins the worker. There is no confirmed on-device expiry mechanism
to cover process crashes or forced termination.

Reserved trigger parameters are rejected. No persistent controller settings
are changed. The cooperative per-container ownership claim does not prevent
the official Betop application from concurrently writing to the device.

Build `testbetophaptics_protocol` and `testbetophaptics_api` with SDL tests
enabled for no-device regression checks. `testbetophaptics --list` performs
enumeration only; `--sequence` sends six bounded physical actuator probes.
Place the built SDL3 DLL on PATH or beside these executables.

Windows x64 / `20bc:5174` receiver enumeration and six-channel host output were
tested on 2026-11-02. User feedback confirmed grip vibration, but no perceived
shoulder/trigger pulse. Those outputs remain unvalidated. `--trigger-test` is a
bounded 20-second diagnostic: pressing a trigger requests moderate resistance
with a renewed 100 ms lease; releasing it requests stop. This is a device
diagnostic, not an in-game bow-state detector.

The subsequent interactive resistance test used background joystick events.
On 2026-11-02 the user confirmed resistance on both triggers and normal recovery
after the test ended. This validates resistance mode, not trigger vibration,
breakthrough mode, shoulder effects, or in-game bow detection.

The local research snapshot at `build/betop-research/2026-11-02/index.html`
contains 65 official catalog details, 100 effect-guide images, installed vendor
artifacts with hashes, and an authored Breath of the Wild adaptation proposal.
It is ignored research material, not source or redistributable SDL content.
Catalog coverage does not mean all native per-game implementations are recovered.

Follow-up shoulder-only testing on 2026-11-02 failed perceptual validation:
left/right, both light touch and held down, produced no perceived effect.
Each stage sent three 300 ms low-crisp presets, with host exit status zero.
The preset bytes match the retained vendor preset builder, but the vendor also
uses a distinct direct-drive command. Whether a preset alone starts physical
output remains unresolved; do not treat these presets as validated actuation.

An opt-in direct-drive diagnostic now uses shoulder preset selector 4 with
strength 255 (on) or 0 (off). These are the observed vendor call-site values;
intermediate values are rejected rather than assumed to be linear amplitude.
The direct report uses `02 76 01`, selector 4/5, and the on/off byte at offset 4.
Vendor builder `0x1401a28c0` and Witcher 3 call `0x1402887ec` establish this
path; the existing preset path uses selectors 0/1. This is not proof that the
preset requires a subsequent direct command, nor a confirmed root cause.
`--shoulder-direct-left` / `--shoulder-direct-right` send three 100 ms pulses
with finite leases and explicit stops. On 2026-11-02 the user reported clear
feedback for both left and right direct-drive tests, with fingers resting on
the shoulders. Both stages exited successfully. Physical direct-drive output
is confirmed; the user subsequently confirmed both shoulders stopped with no
residual vibration. The earlier preset-only tests remain unsuccessful.
Protocol checks (1271) and virtual API checks (134) passed after this addition.

## Actuator-path audit, 2026-11-02

- Grips: native builder `0x1401a23c0` uses `02 16`, with separate left/right
  bytes at offsets 2/3. The local encoder matches this layout, and grip output
  was felt in physical testing. Per-side max(low, high) is an authored amplitude
  reduction, not a reconstruction of Switch frequencies or a vendor mixing rule.
- Adaptive triggers: native builder `0x1401a1cc0` uses `02 56`, side at offset 2,
  mode at 3, and mode-specific fields from 4. Modes 1/2/3/4 use 2/4/3/4 primary
  fields respectively. The vendor also writes two optional tail fields; this
  implementation intentionally supports only their zero-valued form and rejects
  other tail values. Modes must not be advertised as a complete vendor API.
- Trigger resistance (mode 1): both sides and normal stop physically confirmed.
  Vibration (mode 2), breakthrough (mode 3) and follow (mode 4) match the primary
  field layouts but have no positive physical result yet. An unpressed, brief
  pulse does not validate a mode whose sensation depends on trigger travel.
- Shoulders: direct-drive selectors 4/5 with 255/0 are physically confirmed,
  including stopping. The preset selectors 0/1 were not perceptible. Game event
  output has been switched to direct drive; no unsupported texture or linear
  strength mapping is inferred from that binary on/off operation.
- Grip and enhanced-effect leases are independent. Enhanced output combines
  both triggers and shoulders, so clients must submit the complete desired
  enhanced state; an all-zero call stops all four, not just one actuator.
  A worker clears expired leases and close flushes neutral reports. Forced
  termination still has no confirmed firmware-side timeout guarantee.

`--trigger-vibration-left/right` and `--trigger-breakthrough-left/right`
(use the full suffix in each option) are 12-second, single-trigger diagnostics.
Vibration enables the selected mode above the input threshold and stops on
release. Breakthrough now stays armed throughout the stage, avoiding a host
threshold crossing before the device receives its mechanical effect. Both
renew a 100 ms lease and stop on BACK/VIEW, timeout or close. Other actuators stay off.
These test modes do not establish an in-game bow/weapon state.

The first per-trigger audit confirmed clear mode-2 vibration on both sides,
with normal recovery after release and completion. Mode 3 was weak on the left
and felt out of sync on the right; both stopped normally. These results do not
validate breakthrough timing. The initial diagnostic gated mode 3 at about
10% input travel, potentially arming after the intended mechanical threshold.
The revised test changes only this timing policy, keeping start/strength/range
at 10/25/20. Its prearmed behavior still requires physical verification.

In the prearmed follow-up, both triggers initially responded, then lost the
perceived effect; both stopped normally. The vendor queue `0x1401a39e0`
re-enqueues unchanged reports after 2,000,000,000 clock units. Clock helper
`0x14019f940` converts the performance counter to nanoseconds, establishing a
two-second refresh. The local implementation previously deduplicated identical
enhanced reports indefinitely, despite renewing the host lease. Sustained
trigger modes and direct-drive shoulders now refresh at two seconds while the
application continues submitting a valid lease. Stops remain immediate; this
does not renew expired effects or periodically replay unvalidated presets.
Regression checks cover
refresh timing, changed reports, and suppression of repeated neutral/preset
reports. In the subsequent 12-second per-side physical test on 2026-11-02,
the user confirmed both triggers were clear and responsive, and recovered
normally. Start/strength/range remained 10/25/20; adding periodic refresh to
the prearmed test resolved the reported disappearing effect in this trial.
This does not establish a firmware timeout value or reliability beyond the
tested duration. Follow mode 4 and preset-only shoulders remain unvalidated.
