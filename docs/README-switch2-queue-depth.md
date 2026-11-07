# Switch 2 Pro USB output queue-depth comparison

Measured on Windows on 2026-11-06, using one connected Nintendo 057e:2069
controller and the production asynchronous OUT implementation. Keep the default
at **two in-flight requests**. One is insufficient; three and four provided no
additional capacity for the tested 4 ms and 5 ms changing-state workloads.

## Device and measurement boundary

The device's UCX rundown reports `DeviceSpeed=1` (`UsbFullSpeed`), `bcdUSB=0x200`.
The matched interrupt OUT endpoint is `0x01`, maximum packet 64 bytes,
`bInterval=4`. The selected interface uses WinUSB, version 10.0.26100.9750.
`bcdUSB=0x200` denotes USB specification compliance, not negotiated High-Speed.

This endpoint is serviced on a 4 ms schedule on this Windows setup. Its roughly
250 successful reports/second is unrelated to mouse/keyboard High-Speed IN
endpoints offering 125/250 microsecond service (8/4 kHz). USB connector generation,
input report rate, output report rate, and actuator vibration frequency are
different quantities. The four amplitude/frequency components share one report.

References: [Microsoft endpoint descriptor](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/usbspec/ns-usbspec-_usb_endpoint_descriptor),
[Microsoft speed enumeration](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/usbspec/ne-usbspec-_usb_device_speed),
[Razer High-Speed USB polling explanation](https://www.razer.com/ca-en/technology/razer-hyperpolling).

Timing below measures source API entry to **host kernel USB completion**, not a
wire ACK, firmware execution time, motor onset, or perceived intensity. User and
kernel requests match one-to-one in submission order with a constant clock
offset; maximum dispatch residual is checked below 1 ms. Clock alignment uses
median dispatch offset, so absolute latency includes a small alignment error.
Raw captures include no game assets, saves, or game-scene playback.

## Method

One DLL, identical packets and code paths, varying only private diagnostic hint
`SDL_JOYSTICK_HIDAPI_SWITCH2_RUMBLE_SLOTS` from 1 through 4. The default remains 2;
invalid values fall back to 2. Fixed buffers remain immutable until completion.
Submission identity and publication timestamps distinguish source, queue,
kernel, and callback delays; an overwritten pending state is not called a
successfully delivered update.

Three rounds with interleaved depth order: `2,1,4,3`, `3,4,1,2`, `4,2,3,1`.
Each run uses:

- 750 changing stereo publications at 4 ms, then 600 at 5 ms.
- Six isolated pulses at each width: 1, 2, 4, 5, 8, 12 ms (18 per width/depth).
- Twelve stop trials immediately after four rapid changing publications,
  0.2 ms apart, followed by 30 ms service (36 stop trials/depth).
- Low amplitudes 0.015 to 0.05; frequencies 160/320 and 180/360 Hz. Input sensors
  remain enabled and SDL input updates continue during playback.

UCX and USBXHCI ETW capture validates actual OUT endpoint/request identity.
All twelve captures have zero lost events/buffers. No physical unplug is needed
for this comparison; actual unplug/replug acceptance remains deferred.

## Changing-state results

Latency is pooled source-to-kernel completion in milliseconds; percentile values
use successful updates only. Depth 1 therefore needs its omitted-state count to
interpret latency fairly.

| Limit | 4 ms successful / produced | Merged before submit | Latency mean / P95 / P99 | Completion gap mean / max | 6–10 ms gaps |
| ---: | ---: | ---: | --- | --- | ---: |
| 1 | 1128 / 2250 | 1122 | 9.539 / 10.673 / 10.742 | 8.000 / 8.125 | 1125 |
| 2 | 2250 / 2250 | 0 | 5.414 / 6.503 / 6.516 | 4.000 / 4.060 | 0 |
| 3 | 2250 / 2250 | 0 | 4.353 / 4.606 / 4.618 | 4.000 / 4.016 | 0 |
| 4 | 2250 / 2250 | 0 | 5.594 / 7.001 / 7.017 | 4.000 / 4.118 | 0 |

| Limit | 5 ms successful / produced | Merged before submit | Latency mean / P95 / P99 | Completion gap mean / max | 6–10 ms gaps |
| ---: | ---: | ---: | --- | --- | ---: |
| 1 | 1126 / 1800 | 674 | 10.589 / 12.810 / 12.866 | 8.015 / 16.003 | 1120 |
| 2 | 1800 / 1800 | 0 | 6.119 / 7.921 / 7.976 | 5.002 / 8.012 | 450 |
| 3 | 1800 / 1800 | 0 | 6.071 / 7.956 / 8.029 | 5.000 / 8.010 | 449 |
| 4 | 1800 / 1800 | 0 | 5.982 / 7.917 / 7.975 | 5.002 / 8.014 | 450 |

For depths 2–4, maximum observed in-flight count in **both changing-state
workloads was 2**, even when the configured limit was 3 or 4. The spare third
and fourth buffers were never needed. Differences in 4 ms absolute latency
cannot be attributed to extra slots: input and USB service phases differ between
process runs, and phase persists for a 4 ms input. Depth 3's lower sample latency
is not evidence of a three-request performance improvement.

For 5 ms input, around one quarter of completion intervals being 8 ms is expected
when 200 Hz updates occupy 250 Hz service opportunities. Depths 2–4 have no
missing source states and no intervals exceeding source spacing by more than
4.5 ms. Depth 1 has three intervals above 10 ms in its 5 ms segment; its third
round also had source scheduling delays, including a 32.685 ms maximum in the
4 ms segment. These are reported rather than attributed entirely to USB.

Generation-to-submission P99 for depths 2/3/4 is respectively 0.038/0.040/0.036 ms
at 4 ms input, and 0.061/0.069/0.054 ms at 5 ms input. There is no healthy
software 4 ms pacing gate.

## Pulses and stop

Each cell is the number of pulses with at least one successful active report,
out of 18. All active pulses were submitted; unsuccessful ones received
cancellation rather than being overwritten in the latest-state mailbox.

| Limit | 1 ms | 2 ms | 4 ms | 5 ms | 8 ms | 12 ms |
| ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| 1 | 4 | 10 | 18 | 18 | 18 | 18 |
| 2 | 5 | 7 | 18 | 18 | 18 | 18 |
| 3 | 7 | 8 | 18 | 18 | 18 | 18 |
| 4 | 3 | 9 | 18 | 18 | 18 | 18 |

These small phase-dependent samples do not establish a ranking for 1–2 ms
pulses. More slots do not make a 4 ms endpoint execute 1 ms commands reliably.
Cancellation is not proof that no bytes reached the device. Likewise, success
for a 4 ms pulse does not prove an exact 4 ms actuator waveform: in these runs,
the active request's kernel completion occurred after the source requested stop.
Widths 8/12 ms completed active output before the stop call in all trials.
No pulse stretching or new short-effect playback policy was silently introduced.

Full-burst stop latency is stop API entry to successful zero report completion:

| Limit | Trials | Mean | P95 | P99 | Maximum |
| ---: | ---: | ---: | ---: | ---: | ---: |
| 1 | 36 | 4.917 | 8.211 | 8.445 | 8.507 |
| 2 | 36 | 3.021 | 8.329 | 8.392 | 8.414 |
| 3 | 36 | 2.746 | 5.108 | 8.449 | 8.614 |
| 4 | 36 | 3.048 | 8.244 | 8.275 | 8.290 |

All stop reports succeeded after older callbacks drained. Submission/retirement
ordinals, firmware sequence modulo 16, configured bounds, successful kernel
completion order, and final silence passed in all captures. Cancellation cannot
be assumed instantaneous; adding slots did not remove the approximately 8 ms
stop tail. No human motor-silence or physical unplug verification is claimed.

## Decision, regressions, and reproduction

Keep two as the production default: it is the smallest tested limit with full
delivery at both source rates and no extra 4 ms workload completion gaps. Limits
3/4 add burst queue capacity without demonstrated steady-state benefit. This is
a result for this controller/Windows configuration, not a USB-wide optimal depth
or hard real-time guarantee under untested system load.

Production-code fake-libusb regression passes at every depth, including bounded
coalescing, reversed callbacks/FIFO retirement, stop-before-reactivation, NO_DEVICE
drain, and allocation/free balance. Existing default-depth failure/short-stop,
delayed cancellation lifetime and disconnect tests pass. Virtual public API
checks (134) and codec checks (all 1024 codes) also pass. Python compilation and
`git diff --check` pass. No games/emulator were launched for this comparison.

The isolated hardware-test DLL SHA-256 is
`D070BDB6FD6BFF19FF3DFB6543725B52E4DB94B82A3CEBB5A820E37BB57A43E8`.
At comparison time application DLLs retained the previous two-slot version.
The subsequent production-final deployment is recorded in
[the research log](README-switch2-hd-rumble-research.md); it retains this selected default. Captures and pooled results are ignored
local artifacts under `build/rumble-slots/`, including `comparison.json` and each
run's `*-analysis.json`, source log, ETL, and XML. A comment documenting this
decision was added after the measured binary was built; it changes no behaviour.

Build from the SDL repository root:

```powershell
cmake --build build/windows-x64-release --config Release --target SDL3-shared testswitch2async testswitch2async_bench --parallel 12
ctest --test-dir build/windows-x64-release -C Release -R '^testswitch2async$' --output-on-failure
New-Item -ItemType Directory -Path build/rumble-slots -ErrorAction SilentlyContinue
Copy-Item build/windows-x64-release/Release/SDL3.dll build/rumble-slots/
Copy-Item build/windows-x64-release/test/Release/testswitch2async_bench.exe build/rumble-slots/
Copy-Item build/deps/libusb-1.0.30/VS2022/MS64/dll/libusb-1.0.dll build/rumble-slots/
```

Physical playback requires explicit `-Play`, one run at a time. The runner
refuses to overwrite an existing run log; choose fresh round numbers when
repeating. ETW collection must be permitted on the host. The used round orders:

```powershell
./test/run_switch2_slots.ps1 -Play -Round 1 -Slots 2,1,4,3
./test/run_switch2_slots.ps1 -Play -Round 2 -Slots 3,4,1,2
./test/run_switch2_slots.ps1 -Play -Round 3 -Slots 4,2,3,1
../totk-pc-recomp/.venv/Scripts/python.exe test/analyze_switch2_slots.py build/rumble-slots --xml-wall-offset-minutes 480 --runs r1-s2 r1-s1 r1-s4 r1-s3 r2-s3 r2-s4 r2-s1 r2-s2 r3-s4 r3-s2 r3-s3 r3-s1
```

The explicit 480-minute XML wall offset is required for this host's malformed
tracerpt timezone suffix; it must be validated for other hosts.
