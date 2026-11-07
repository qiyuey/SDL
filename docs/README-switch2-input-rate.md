# Switch 2 Pro raw input rate: 250 Hz versus 500 Hz

## Result on the attached controller

On 2026-11-06, the attached Nintendo 057e:2069 controller produced approximately
**250 raw USB interrupt IN reports/second**, both before and after the user moved
it to a motherboard USB port. Native Windows HID reading and an isolated report
0x09 format contrast also remained approximately 250 Hz. No physical 500 Hz IN
stream was reproduced. This is a result for this controller and host setup, not
proof that every firmware, driver, or console configuration behaves identically.

The device rundown shows Full-Speed (`DeviceSpeed=1`), `bcdUSB=0x200` and
`bcdDevice=0x0201`. The actual input endpoint is interrupt IN `0x81`, 64-byte max
packet, `bInterval=4`. This is separate from interrupt OUT `0x01` used for rumble.
The motherboard captures retained the same speed and endpoint parameters.

All captures used the physical device, not a virtual XInput gamepad or browser
tester. No Steam, game, or emulator was launched, no driver was installed, and no
rumble API or nonzero vibration effect was issued. The existing SDL startup and
sensor commands initialized the device's volatile reporting state.

## Capture evidence

The SDL diagnostic tap logs every 64-byte report before gameplay/sensor parsing.
UCX/USBXHCI ETW independently pairs physical interrupt IN requests by IRP/URB,
filters the target device and endpoint, and excludes vendor bulk ACK traffic.
Native HID uses read-only `ReadFile` with two pending buffers, without SDL input
parsing. This rules out counting multiple SDL events as multiple USB reports.

| Capture | Raw report ID | Raw count in steady window | Steady duration | Whole-capture kernel mean interval | Kernel mean rate |
| --- | --- | ---: | ---: | ---: | ---: |
| Before connector change, SDL | 0x05 | 1500 | 6.0005 s | 4.000157 ms | 249.990 Hz |
| Motherboard, SDL round 1 | 0x05 | 1501 | 6.0007 s | 4.000126 ms | 249.992 Hz |
| Motherboard, native Windows HID | 0x05 | 1501 | 6.0006 s | 4.000124 ms | 249.992 Hz |
| Motherboard, native-report format contrast | 0x09 | 1500 | 6.0005 s | 4.000124 ms | 249.992 Hz |
| Motherboard, restored normal SDL round 2 | 0x05 | 1500 | 6.0003 s | 4.000125 ms | 249.992 Hz |

Boundary-inclusive counts can differ by one; mean frequency comes from kernel
completion timestamps rather than rounding a six-second count. All five traces
have zero ETW event/buffer loss. Full-capture physical IN completion counts are
2744, 2724, 1507, 2746 and 2746 respectively. Startup includes input received
before the SDL parser is opened, so full-session raw/parser counts are not
assumed equal. In the anchored final normal 6-second window, exactly 1500 raw
reports match exactly 1500 kernel IN completions.

The restored motherboard run's whole-capture kernel interval P95/P99 is
4.0068/4.0116 ms, range 3.0587–4.9396 ms. The report-0x09 contrast's range is
3.9111–4.0891 ms. There is no sustained 2 ms physical IN stream in these samples.
ETW completions are host-stack observations, not electrical ACK measurements.

For normal report 0x05, the four-byte field after the report ID advanced by 4
between each steady-window packet, and adjacent whole packets were never
identical. The raw sensor timestamp field at USB offset 0x2b also changed. Its
observed steps included 193, 145 and 144; its units were not assumed to be
microseconds, and the existing SDL calibrated/scaled timestamp was not used to
infer hardware frequency. Counter increments alone do not prove that extra USB
reports were transmitted between the observed completions.

## Why software can show 500 events/second

The final normal run emitted 1500 gyro events and 1500 accelerometer events from
1500 raw reports during the same six-second interval:

| Quantity | Approximate rate |
| --- | ---: |
| Raw USB reports | 250 Hz |
| Gyroscope events alone | 250 Hz |
| Accelerometer events alone | 250 Hz |
| Both sensor event types added together | 500 events/s |

Thus a 500-event/s software counter need not mean 500 new USB packets/s. This is
a demonstrated counting distinction, not an allegation that an external tester
made that mistake. Axis events are value-change events per axis and likewise
cannot directly measure transport rate.

Report 0x09 has a different packed motion layout. The diagnostic contrast logs
it without feeding it to the normal 0x05 parser, so its zero SDL sensor-event
counts are intentional and say nothing about whether the physical IMU is active.
No claim about its internal IMU sample frequency is made from packet frequency.
Multiple samples in a packet or a higher internal sensor sample rate would not
by itself increase physical USB packet or rumble OUT frequency.

## What the public 500 Hz reports actually describe

1. A Steam user initially reported 250 Hz gyro-to-mouse output, then corrected
   it to 500 Hz after replacing a USB hub connection with a direct connection.
   The stated measurement was a mouse tester; the author could not measure the
   controller's stick rate with the gamepad tools then available. No physical
   USB capture or driver interval override was supplied.
   [Original account and updates](https://www.reddit.com/r/GyroGaming/comments/1p766os/switch_pro_controller_2_has_now_been_added/).
2. Another user reported steady 500 Hz sticks through Steam and approximately
   500 Hz gyro. The post supplies neither a raw USB trace nor a complete tool,
   firmware, driver and configuration recipe.
   [First-person report](https://www.reddit.com/r/pcmasterrace/comments/1vl9cpd/switch_2_pro_controller_pc_polling_rate/).
3. Switch2Connect advertises wired support up to 500 Hz. Source was inspected
   read-only at commit `e5dd90bb54106b35a1110f96ec9d060390e5d096`; no GPL source was
   copied into SDL. Its wired reader uses blocking native HID reads of 64 bytes
   with an 8 ms timeout, measures read-return deltas and forwards reports to a
   translation callback. That timeout is not an 8 ms polling scheduler. The
   inspected reader does not implement a physical IN interval override.
   [Pinned wired reader](https://github.com/TommyWabg/Switch2Connect/blob/e5dd90bb54106b35a1110f96ec9d060390e5d096/src/usb_hid_controller.py).
   Its initialization selects report 0x09, whereas our SDL path selects 0x05.
   We independently tested that one documented report-format difference; it did
   not produce 500 Hz. Its controller layer also has a separate interpolating
   gyro/joystick-to-mouse worker. Mouse-output frequency therefore needs separate
   verification from physical input frequency.
   [Pinned initialization and mouse worker](https://github.com/TommyWabg/Switch2Connect/blob/e5dd90bb54106b35a1110f96ec9d060390e5d096/src/controller.py).
   The full application, virtual drivers and complete external initialization
   sequence were not executed. This inspection does not establish why a given
   external tester observed 500 Hz.

The report-format contrast uses only the documented volatile command 03:0a to
select input report 0x09. The final normal run selects 0x05 again and verifies
normal events. Pairing, flash writes, firmware updates and driver overrides were
not involved. Protocol references are reverse-engineered third-party material,
not Nintendo specifications:
[report selection](https://github.com/ndeadly/switch2_controller_research/blob/master/commands.md#subcommand-0x0a---select-input-report),
[report layouts](https://github.com/ndeadly/switch2_controller_research/blob/master/hid_reports.md).

The reproducible conclusion is **250 physical IN packets/s on this setup**.
There is currently no verified external procedure from these sources that we
can claim raises this controller's physical IN stream to 500 Hz. Nor can input
claims establish a 2 ms or 1 ms rumble OUT capability.

## Reproduction and checks

Private hints enable raw tracing and the optional raw-only report-0x09 contrast.
They are off in normal use. Normal gameplay parsing still defaults to 0x05.
This capture task did not deploy DLLs to application directories. Captures and analysis JSON are ignored artifacts
under `build/switch2-input/`; per-report hex is intentionally kept out of this
document. The final diagnostic DLL SHA-256 is
`8955FA53E03A64C38A62F687F7595E06726A069003FC2D1BF84779B1C5C22B9F`.
The first two normal captures predated the wall-clock anchor addition, so only
their global kernel interval statistics are compared; the final normal and 0x09
steady windows have explicit anchors and raw/kernel count agreement.

```powershell
cmake --build build/windows-x64-release --config Release --target SDL3-shared testswitch2input testswitch2input_native --parallel 12
New-Item -ItemType Directory -Path build/switch2-input -ErrorAction SilentlyContinue
Copy-Item build/windows-x64-release/Release/SDL3.dll build/switch2-input/
Copy-Item build/windows-x64-release/test/Release/testswitch2input.exe,build/windows-x64-release/test/Release/testswitch2input_native.exe build/switch2-input/
Copy-Item build/deps/libusb-1.0.30/VS2022/MS64/dll/libusb-1.0.dll build/switch2-input/
./test/run_switch2_input.ps1 -Name fresh-normal -Mode sdl
./test/run_switch2_input.ps1 -Name fresh-native -Mode native
./test/run_switch2_input.ps1 -Name fresh-report09 -Mode report09
./test/run_switch2_input.ps1 -Name fresh-restored -Mode sdl
../totk-pc-recomp/.venv/Scripts/python.exe test/analyze_switch2_input.py build/switch2-input --xml-wall-offset-minutes 480 --runs fresh-normal fresh-native fresh-report09 fresh-restored
```

Run one capture at a time. Native HID relies on the previous SDL run's volatile
initialization and performs no output writes itself. Repeating after unplugging
requires the SDL initialization run again. Always finish a 0x09 contrast with a
normal run to restore 0x05. The runner refuses to overwrite an existing log and
requires ETW collection permissions. The explicit XML wall offset is host-specific.

Validation: production async CTest passed; 134 virtual haptics API checks and all
1024 codec values passed with the diagnostic DLL; analyzer Python compilation
and source whitespace checks passed. No motor response or physical-unplug
acceptance is inferred from this input-only observation.
