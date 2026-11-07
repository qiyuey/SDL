# Switch 2 Pro HD Rumble 2 适配研究

记录日期：2026-11-05（Asia/Shanghai）。本文是本地适配研究记录，不是已完成的驱动规格。

## 2026-11-06 强度接口更新

强度只在应用层缩放一次：Switch 内容 100%=5×，Switch 2 内容 100%=1×。
SDL3 的频率 API 接收最终的 0–1 振幅，不推断游戏类型，也不再次放大。
`SDL_JOYSTICK_HIDAPI_SWITCH2_HD_GAIN` 已停止读取；下文关于该增益的记录属于历史实验。
公共 API 签名、频率编码候选公式与输出调度不变。原生 HD Rumble 2 模式仍等待 Switch 2 HID 模块。
本次仅修改源码，未编译或部署 DLL。详见 [强度约定](README-rumble-strength.md)。

## 目标与当前结论

目标是让游戏经自定义 SDL3 向 Switch 2 Pro 输出左右独立的低频频率、低频振幅、高频频率、高频振幅，以及正确的更新、持续和停止行为。当前先完成 USB；蓝牙另需 BLE 连接支持。

- **已实测**：补齐 libusb 后，Windows 上的有线 Switch 2 Pro 可以被 SDL3 打开；加速度、陀螺仪有有效数据；两次短震动正常并完全停止。
- **原有驱动**：普通低/高频强度接口使用固定频率码，Pro 左右发送相同参数。现已增加实验性的左右独立频率接口，尚未完成物理频率标定，不宣称准确复现。
- **新发现**：任天堂专利提供了明确的对数频率和线性振幅编码公式，值得优先验证。
- **尚未确认**：专利实施例是否就是实物采用的编码；字段最高位含义；两组振动分量与低/高频命名的对应；振幅的频率响应补偿。

不要将“能震动”“支持频率字段”或“手感更丰富”等同于准确重现原游戏 HD 震动。

## 1. 关键一手资料

| 来源 | 用途与证据边界 |
| --- | --- |
| [任天堂 US20260000974A1](https://patents.google.com/patent/US20260000974A1/en) | 具体实施例的频率/振幅编码与解码、音阶关系、频率响应电压限制。重点读公式 A–D、图 3–5、图 9。公开专利不自动证明量产手柄固件采用同一实现。 |
| [Switch 2 Controller Research：Bluetooth](https://github.com/ndeadly/switch2_controller_research/blob/master/bluetooth_interface.md) | BLE 广播、自定义 GATT、连接与配对流程、输入及震动特征 UUID。 |
| [Switch 2 Controller Research：Commands](https://github.com/ndeadly/switch2_controller_research/blob/master/commands.md) | 命令初始化及振动命令；`0x0A/0x08` 的说明仍标记原始数据格式未完全理解。 |
| [一代蓝牙 HID 报告](https://github.com/dekuNukem/Nintendo_Switch_Reverse_Engineering/blob/master/bluetooth_hid_notes.md) | 一代 `0x10`/`0x01` 输出报告、每侧 4 字节震动格式。 |
| [一代频率与振幅表](https://github.com/dekuNukem/Nintendo_Switch_Reverse_Engineering/blob/master/rumble_data_table.md) | 一代频率对数编码和非线性振幅曲线；不能直接当作二代协议使用。 |

发现专利的线索来自 [HD Rumble 2 Data Encoding](https://ninpatentswatch.com/2026/01/04/hd-rumble-data-encoding/)。技术判断应回到上表中的专利原文，而不是只采用博客解释。

## 2. 专利候选映射：尚待实物验证

专利具体实施例的公式 A–D 为：

```text
x = 96 * log2(f_hz / 110) + 333
f_hz = 110 * 2^((x - 333) / 96)

y = amplitude * 1023
amplitude = y / 1023
```

该实施例频率和振幅均为 10 位，编码范围为 0–1023。频率量化要比较相邻编码解码后的 Hz，选择更接近目标者，不能只凭整数截断。零振幅、无效浮点数、超范围频率的 API 行为仍需另外定义和测试。

| 目标频率 | 公式实数结果 | 附近整数码 |
| --- | --- | --- |
| 40 Hz | 192.895 | 193 |
| 80 Hz | 288.895 | 289 |
| 160 Hz | 384.895 | 385 |
| 320 Hz | 480.895 | 481 |
| 400 Hz | 511.800 | 512 |

**这些是公式计算值，不是测量值。** 400 Hz 附近跨过 511/512，尤其适合核对第 10 位究竟是频率连续编码的一部分还是独立模式位。

按候选公式反算，SDL 当前固定码 `0x112`（274）对应约 71.843 Hz，`0x187`（391）对应约 167.211 Hz；社区常用 `0x0E1`（225）对应约 50.435 Hz，`0x1E1`（481）对应约 320.244 Hz。这些只能作为验证假设，不能宣称手柄当前实际输出这些频率。

线性振幅编码也不意味着两代手柄在相同数字下震感相同：专利还讨论频率相关的最大允许驱动电压。不要将编码满量程直接作为测试输出强度。

## 3. 两代蓝牙及震动格式差异

| 项目 | Switch Pro | Switch 2 Pro |
| --- | --- | --- |
| 传输 | 经典蓝牙 BR/EDR HID | BLE，自定义 GATT，非标准 HID over GATT |
| 输入 | HID 报告，例如 `0x30` | 按不同 GATT 特征订阅通知 |
| 震动输出 | `0x10`，或 `0x01` 随子命令发送 | 专用 GATT 写入；USB 有独立封装 |
| 常用每侧参数载荷 | 4 字节 | 5 字节参数单元，另有控制/序号与封装 |

Switch 2 Pro 的 BLE 震动特征 UUID：`cc483f51-9258-427d-a939-630c31f72b05`。NS2ProCon 当前 BLE 输出为 33 字节，左右各一个 16 字节区域；不要直接用于 USB，当前 SDL USB 输出是 64 字节。

**字段解释存在冲突**：SDL 的 `EncodeHDRumble()` 按两组 10 位频率/10 位振幅打包；NS2ProCon、Switch2Connect 将相应频率控制区域解释为 9 位频率加 1 位 tone 标志。字段顺序的低/高频命名也不同。适配前应以原始位偏移对照，不能仅凭变量名交换两组分量。

## 4. 现成项目的参考价值

### Switch2Connect

检查版本：`e5dd90bb54106b35a1110f96ec9d060390e5d096`。

- [controller.py](https://github.com/TommyWabg/Switch2Connect/blob/e5dd90bb54106b35a1110f96ec9d060390e5d096/src/controller.py)：`VibrationData.get_bytes()`、`set_vibration()`；可核对左右独立参数、字段打包和输出调度。
- [virtual_controller.py](https://github.com/TommyWabg/Switch2Connect/blob/e5dd90bb54106b35a1110f96ec9d060390e5d096/src/virtual_controller.py)：`decode_32bit_rumble()` 提取一代编码字段、解码振幅。
- Switch1 模拟路径包含 `Artificial Frequency Expander`：Pro 高频码乘以 `4.167`，较大一段低频码范围改为固定 `0x0E1`。这是人为调参，不能作为准确 Hz 映射的证据。
- 参考协议事实和行为，不在未核实许可的情况下复制代码到 SDL。

### 其他实现

| 项目 | 已观察到的行为 | 使用边界 |
| --- | --- | --- |
| [NS2ProCon](https://github.com/ErenYaeger999/NS2ProCon/blob/main/src/NS2Pro.Core/HapticProtocol.cs) | 有独立左右参数结构；默认载波码为 `0x112`、`0x187`；代码说明频率为协议码而非测量 Hz。 | 连接、打包、停止机制有参考价值；Xbox/DualSense 普通反馈路径会损失原始参数。 |
| [switch2-pico](https://github.com/joshbirnholz/switch2-pico) | `s2_rumble_freq_code()` 用中性码加可调斜率乘 `log2(hz/160)`；README 将频率翻译标为低置信度。 | 可参考测试结构；不能把其近似公式作为验证结论。 |
| [S2P-XInput-Lite](https://github.com/duoduo-88/S2P-XInput-Lite) | 用户文档将 0–511 控制码近似解释为 Hz。 | 本次未找到支撑此解释的频谱测量记录；与专利公式不一致。 |
| [wavebird](https://github.com/murphyjt/wavebird) | `SwitchProSession.swift` 明确丢弃一代频率，以固定二代载波输出。 | 不能据“Switch Pro 模式”认定其保留完整频率。 |

这些仓库的后续版本可能变化，以上结论仅对应本次读取的内容。

## 5. 本机 USB 支持修复与验证

环境：Windows x64；真实 Nintendo Switch 2 Pro Controller，VID/PID `057E:2069`；SDL 源码基线 `fa035f0291d3aaf724bfc6260350e9a79a2a5fcb`，保留原有本地 FIX。

原构建虽然请求 `SDL_HIDAPI_LIBUSB=ON`，实际配置仍未定义 `HAVE_LIBUSB`，因此 Switch 2 驱动没有编入。

修复内容：下载 [libusb 官方 1.0.30 Windows 包](https://github.com/libusb/libusb/releases/tag/v1.0.30)，使用 `include/libusb.h` 和 `VS2022/MS64/dll/libusb-1.0.lib` 重新配置构建。**该包头文件位于 `include`，不是 `include/libusb-1.0`。** 配置结果为：

```c
#define HAVE_LIBUSB 1
#define SDL_LIBUSB_DYNAMIC "libusb-1.0.dll"
```

以仓库根目录为工作目录的复现命令：

```powershell
cmake -S . -B build/windows-x64-release `
  "-DLibUSB_INCLUDE_PATH=$PWD/build/deps/libusb-1.0.30/include" `
  "-DLibUSB_LIBRARY=$PWD/build/deps/libusb-1.0.30/VS2022/MS64/dll/libusb-1.0.lib" `
  -DSDL_HIDAPI_LIBUSB=ON -DSDL_HIDAPI_LIBUSB_SHARED=ON
cmake --build build/windows-x64-release --config Release --target SDL3-shared --parallel 16
```

游戏 `../SavageWindRecomp/build/` 已更新 `SDL3.dll`，并放入 `libusb-1.0.dll`；旧库备份为 `SDL3-before-libusb-20261104-235933.dll`。没有安装 USBIP 或 GameInput 系统组件。

实物验证结果：

- 初次打开 USB 通信接口返回拒绝访问；用户完全退出相关程序并重新插拔后成功。Steam 当时在运行，但没有单独隔离证明一定是 Steam 占用。
- 原生测试程序直接加载游戏目录 DLL，使用 `SDL_JOYSTICK_HIDAPI_SWITCH2` 打开设备。
- 持续采样获得非零加速度及陀螺仪数据。
- 普通低频、普通高频分别发送强度 `7000/65535`、时长 250 ms 的测试，API 返回成功。
- 用户确认：“有两次短震动，已停止”。这仅验收普通震动与停止，不验收实际 Hz、左右独立或完整 HD 震动。

构建 SHA-256：

```text
SDL3.dll
ACF22BC5A3BBF3920AD6E56AE49143C0D6E0347322EB1E8FDDCA5E691473BA57
libusb-1.0.dll
7CBF37E76DAE9C840C7E8DBF7348EE8897DCC86C8BA45E46ADA60B89411569F7
```

辅助记录位于 `../SavageWindRecomp/build/switch2-usb-validation.json`，测试程序源码为同目录 `switch2-usb-check.c`。这些属于本机忽略的构建产物，不保证其他机器存在。

## 6. 后续实施与验收

1. 核对原始 40 位参数单元的字段顺序及第 10 位含义；优先验证专利公式，保留不同解释作为假设。
2. 先用低强度、短时输出验证候选码；以频谱或其他可复现测量核对 Hz。手感确认只能证明有输出及区别，不能标定 Hz。
3. 保留左右各自的两个频率和两个振幅，不经过 XInput/DualSense 普通双电机反馈转换。
4. 将确认后的映射接入 `RumbleJoystickFrequenciesEx`，保留普通震动兼容、量化边界和有限值检查。
5. 验证持续刷新、短脉冲、零振幅、时限到期、断开、失焦及退出时停止；避免旧反馈覆盖停止。
6. 在真实游戏中分别验证左右独立效果及频率变化，记录设备、平台、测试输入、观察结果和限制，再宣称完整适配。

截至本记录：尚未进行频率标定录音；用户最新要求是先搜索并整理现有资料。不要将之前的测量方式询问当成已授权录音。

## 7. 专利公式实验分支进展

随后用户授权按专利公式继续适配并配合实物测试。当前工作区增加：

- `src/joystick/hidapi/SDL_switch2_rumble.h`：候选频率转换、保留原有强度上限的振幅转换、40 位打包。
- Switch 2 Pro 的 `RumbleJoystickFrequenciesEx`：左右独立参数、持续刷新；普通震动或到期归零会退出独立模式。
- 实验开关 `SDL_JOYSTICK_HIDAPI_SWITCH2_EXPERIMENTAL_HD=1`。默认不启用；测试程序只在自身进程内启用，不改全局环境或游戏启动脚本。
- `test/testswitch2rumble.c`：无手柄测试通过专利示例、全部 1024 个码往返、四个字段互不串位、与旧 SDL 编码器逐字节一致性。

源码位运算已确认完整保留频率第 10 位，不能据此断言硬件一定按专利解码该位。保持上游第一组称为 high、第二组称为 low 的约定，物理对应仍待确认。

首轮实验按左160、右160、左80、左320、左400 Hz 候选顺序发送，均使用 low 参数分量，high 分量振幅为零；每段 400 ms、间隔约 1 s，振幅为 API 的 0.16，保留旧驱动约 0.44 满量程上限。API 均成功，用户反馈“感受到了，但是感觉挺弱的都”。**该反馈不构成左右区分、频率正确或每段停止的明确验收。** 下一轮拟提高到 0.35，其他条件不变。

尚未采集音频、尚未证明实际 Hz。switch2-pico 的打包采用相同两组 10 位字段，但其 `freq_slope` 近似公式并非专利公式，不混用二者。

交互 CLI 随后完成六项 35% 振幅测试（增加了左侧第一组分量的 320 Hz 候选）。用户反馈“能感受到频率的不同，但是强度感觉太低了”；这支持存在可感知差异，但仍不证明实际 Hz，左右区分及本轮停止也未获明确逐项反馈。测试器现提供 35/50/70% 振幅、回车触发、`r` 重试和 `q` 退出；默认50%，不更改游戏的全局强度。每次仍为400 ms，保持原有振幅上限。

### 换 USB 接口后的测试

用户手动卸载 USBip、换 USB 接口后再次完成交互测试。最终一次完整记录为：

- 静置10.001秒：加速度、角速度事件各2499次（约250Hz）；角速度三轴绝对峰值分别为0.00291、0.00277、0.00628 rad/s。
- 转动10.001秒：两类事件各2500次；角速度三轴绝对峰值分别为10.62363、5.96975、6.51492 rad/s。
- 六项震动均返回成功；第一项先试50%再试70%，其余使用70%。
- 用户明确补充“能分辨，有区别”，对应左右区分和候选频率差异。没有提供准确Hz测量、各轴方向验收或本轮停止的逐项确认。
- 更早一次转动阶段仅650次事件，不与最终完整记录混用；不能仅据此次更换接口就断定此前丢样根因。

随后修正输出维护：零振幅不再等待下一帧输入才提交；无输入报告时也处理待发送输出；提交失败保留待重试状态；刷新时限从当前时间计算，避免长时间停顿后追赶旧时限。编码回归测试和Release构建通过，新的DLL已更新到游戏目录。这些修正尚需新一轮实物停止验收。

本机游戏实验入口：`../SavageWindRecomp/build/Play-Switch2-HD-experimental.cmd`。仅在该进程启用候选HD映射，忽略先前的蓝牙一代Pro，并开启体感日志。普通 `Play.cmd` 默认仍使用兼容震动。候选公式尚未完成物理频率标定，不标记为完整准确复现。

### 振幅范围更新（当前行为）

用户在游戏中反馈震动过轻，随后明确要求使用完整振幅范围。实验通道现将0–1线性映射并四舍五入到0–1023，取消之前约453的上限；最高输入仍为游戏实际请求的1，不把所有效果强制变成满强度。普通兼容通道不变。

`SDL_JOYSTICK_HIDAPI_SWITCH2_HD_GAIN` 可取1–3，默认1，非法值回退1；实验启动脚本明确设为1，不叠加此前提出的2倍增益。零振幅、半幅、满幅及饱和的编码测试通过。已重编并更新游戏DLL，旧文件备份在游戏build目录的 `SDL3-before-full-amplitude.dll`。更新后的实物强度和游戏体验尚待用户复测；此前低强度测试结果不能直接当作满范围验收。

## 8. 从HD1参数到HD2震感：新的研究目标

用户明确要求尽量还原原游戏震感，而非仅提高强度。用户目前只有Switch 2 Pro，没有原装一代Pro或Joy-Con作对照。因此目前只能验证目标设备上的参数响应与事件保真，不能把主观调参称为已匹配原装一代。

### 资料与适用方向

- [JP2026015701A](https://patents.google.com/patent/JP2026015701A/en)：图4、图5及频率特性调整，描述频率相关电压限制和不同输入振幅对应的补偿曲线，支持按频率和振幅建立二维映射。原文举出140Hz、输入0.1调整为0.17的实施例；这不是可直接套用到本手柄的通用系数。
- [JP2026015702A](https://patents.google.com/patent/JP2026015702A/en)：明确说明曲线Ln1–Ln4及每频率起振阈值来自预先实验。小于参考控制器感知阈值的输入可归零，而非一律抬高弱信号；两组振动分量还需要合成约束。
- [JP2026015703A](https://patents.google.com/patent/JP2026015703A/en)：对较难起振的第二控制器，在从静止起振且频率不高于150Hz等条件下，给出短时1.2倍增益的实施例，示例周期为5ms，也列出其他持续方式。它不能不分控制器、方向和脉冲长度直接加到HD2输出上。

上述专利描述第一/第二控制器的不同特性，不是附带量产型号标定数据的通用“HD1→HD2转换表”。不能直接把某个方向的补偿曲线倒置，也不能用曲线示意图的少量样例伪造完整LUT。

### 当前输入所在层次

游戏运行时通过HID命令201/206交付 `VibrationValue`，`vibration_send()` 保留各侧低/高频及振幅，做有限值和范围处理后传给SDL。此路径没有先生成一代4字节手柄震动包。因此无需再次进行一代线协议振幅解码；仍需检查游戏SDK此前的混合与实际时间序列，不能假定逻辑振幅就是马达驱动电压。

### 拟采用的验证方法

1. 保留源频率、左右空间信息及各分量的起止时间，先确认频率编码本身。
2. 记录真实游戏事件的请求参数，识别常用频率、强弱范围、脉冲长度和两分量同时工作的情况。
3. 标定目标设备的频率/振幅响应、可感知阈值及起振/衰减。若有可信原装HD1响应数据，求在同频率、相近时间包络下响应相近的目标振幅；否则仅称为目标设备调校。
4. 对持续振动使用随频率和振幅变化的曲线；对冲击效果另外核对时间包络。保持零值为停止，不用固定底噪或最低振幅填补所有弱信号。
5. 两组分量同时输出时保持相对关系，不把每侧两组振幅独立拉满后忽略合成；具体限值需数据支持。

实现上暂不增加未经测量的补偿LUT或启动增益，也不擅自撤销用户此前选择的完整振幅范围。完整线性范围仍只是对照基线，并非最终震感映射。

### 参数采集入口

游戏 `runtime/hle/vibration.c` 的可选 `SAVAGEWIND_VIBRATION` 日志现记录单调毫秒时间、handle、两组振幅与Hz，频率单独变化也会记录。它记录的是游戏服务收到的变化请求，不是USB实际送达时间；重复的相同值不逐条记录，也不是高精度马达输出测量。

入口为 `../SavageWindRecomp/build/Play-Switch2-HD-trace.cmd`。仅本次启动启用参数日志，仍使用增益1的实验映射。日志输出在游戏现有 `build/savagewind.log` 中，下一次启动可能覆盖，应在后续分析前保存。本次构建及 `test-vibration` 通过，尚未用新入口采集实际战斗样本；不录音，不启动其他手柄测试。

## 9. 可视化手柄实验室

`test/switch2_haptics_lab.py` 实现截图所示类型的交互测试能力（不是任天堂演示内容或资产的复制）：二维频率/强度面板、50/200/400Hz预设、40–480Hz线性扫频、左右选择、第一/第二分量选择、两分量各半，以及波形示意。

本机启动入口为游戏build目录的 `HD2-Lab.cmd`，使用游戏目录的自定义SDL3和libusb。仅支持USB `057E:2069`。初始不播放，预设和扫频按钮只准备参数，按“开始”后才输出；自由播放最多10秒，扫频8秒。Esc、停止、失焦、断开或关闭取消播放。后台每40ms更新输出并设置120ms租期，UI更新中断超过300ms也请求停止。硬件停止依赖正常设备通信，不能把软件租期当成手柄固件看门狗。

输出默认振幅25%，增益1，完整10位范围；两分量模式各用一半振幅。频率仍为专利候选映射，波形不是测量结果。不调用麦克风、不录音。UI状态测试覆盖初始静音、显式开始、预设取消播放和扫频结束；实际扫频手感仍需用户验证。

实验室现另提供“双频叠加”模式：默认第二组160Hz/25%、第一组320Hz/25%，两组频率和振幅独立发送。二维面板调第二组，上方高频滑条调第一组；“仅低频/仅高频/双频叠加”支持同强度对照，不因独奏而自动放大或归一化。原“同频各半”与此模式不同。波形叠加只作示意，图上固定缩小一半以容纳两组信号，不影响实际输出。已检查左右路由、独立参数、单分量对照、拖拽保持双频、停止和旋律模式隔离；实际双频手感尚待用户测试。

## 10. 实际游戏请求样本分析（2026-11-05）

已保存本机游戏日志到 `../SavageWindRecomp/build/hd2-trace-20261105-135115.log`，避免下次启动覆盖。`test/analyze_switch_rumble_trace.py` 可重复生成统计；本次结果在 `build/hd1-hd2-request-analysis.json`。

两侧各479条变化记录，其中456条活跃。活跃请求的低频均为160Hz；高频参数虽为320Hz，但**高频振幅始终为零**。每侧非零最大分量振幅最小0.005、按变化记录统计的中位数0.04078、最大0.11251；这不是按时间加权的强度，更不是马达加速度。当前满范围线性编码对应非零振幅码约5–115。

合并左右后得到23段有明确停止的完整效果，主要持续约137ms、231ms。每侧有456个正变化间隔短于12ms，中位正间隔9ms。当前驱动12ms节奏及异步写入队列可能合并包络变化，但本日志不能证明USB实际丢了哪些包、损失多少峰值；不能把输入统计当作输出抓包。也不能将本次样本概括为整款游戏的所有震动。

这说明至少本段轻震动并非来自高低频混合失败，下一步应优先核对160Hz弱振幅响应与瞬态包络，而非添加统一增益、强行补上高频或改变游戏原始载波。

### 可重复的原始效果对照

`test/replay_switch_rumble_trace.py` 合并日志两侧时间线，选择最短和最长的完整效果；保留原请求振幅、频率和时间偏移，输入1/2才播放，q退出。结束、异常或Ctrl+C均请求停止；不猜测没有结束记录的效果，不录音。API调度迟到可见，但不是USB传输时间测量，现有驱动限速仍适用。

本机入口为游戏build目录的 `Replay-HD1-Effects.cmd`。测试覆盖两侧不同频率合并、停止边界、未结束样本排除及真实23段提取。尚未进行这两段回放的用户手感确认。原始日志和统计位于忽略的build目录，代码中不嵌入游戏数据。

**本轮没有把专利示意曲线或其他项目的经验倍率写成“官方HD1→HD2 LUT”。** 需要下一步的实际包络对照和响应测量后才能确定补偿参数；现有独立频率、增益1及满振幅范围保持不变。

### 2026-11-05：振幅路径复核与目标设备调校

游戏浮点 VibrationValue 直接经 runtime/hle/vibration.c 进入 SDL。Switch 1 驱动在末端查表编码；Switch 2 实验路径单独执行 round(a*1023)。不能把传入的浮点振幅当作 HD1 协议编码再解码，也不能将编码索引的比例当成驱动力比例。

再次核对 [JP2026015702A](https://patents.google.com/patent/JP2026015702A/en) 图5、图10、图18相关正文：补偿需要随频率和输入振幅变化的曲线及实测起振阈值。图18还给出弱信号插值后抬至目标阈值的变体，但仍需要参考设备阈值。未知数据不能猜填。[JP2026015701A](https://patents.google.com/patent/JP2026015701A/en) 的140 Hz、0.1到0.17是实施例，不能外推成160 Hz通用1.7倍标定。

本地 Switch2Connect 的 decode_32bit_rumble 将解码振幅乘800；该实现不是两代手柄响应实测LUT，不能据此证明1023线性基线偏小。

回测工具现提供可选的平滑动态范围曲线，默认保留原始对照。每侧令 m=max(a_low,a_high)，两个分量同时乘 g/(1+(g-1)*m)，g取1、2、4、8。参数是实验选择，不是专利参数。保持零点、满幅端点、频率、时间、左右独立和同侧分量比例。弱输入提升更多，满幅附近仍有变化；会改变包络的幅值形状和跨强度对比。没有添加起振脉冲或底值，也不代表双频合成电压已标定。

输入0.025/0.06751/0.11251/0.125，在强档g=8时约输出0.170/0.367/0.504/0.533。没有按每项效果分别归一化。GUI显示原始与输出曲线、峰值；切换停止，点击播放才输出。增强仅对Switch 2 Pro开放，尚未改成游戏默认，也未改动SDL DLL。

实现：../SavageWindRecomp/tools/rumble_amplitude_mapping.py 与 rumble_catalog_player.py。四项离线测试、76项效果的四档映射校验及200% DPI界面检查通过，未输出实际震动。下一步实物比较小、中、大、持续小震动的轻重层次和收尾。只有Switch 2 Pro时可确定偏好及可感阈值，不能宣称已复现原装HD1。频率相关补偿还需测量，不从160 Hz推广到所有频率。


## 2026-11-06: Switch 2 Pro 4 ms scheduling source changes

The upstream generic HIDAPI rumble worker's post-write `SDL_Delay(10)` originated
in commit `480c1f9fef73dece0ead7c95a60dcb67ec4e7ce7` (2021-01-01), described as
avoiding starvation of report reads. The Switch 2 driver's separate 12 ms
constant originated in `c89fed4eae488ae5f953b722facb576a22719485`. Neither commit
establishes a hardware minimum output period.

The locally attached 057E:2069 controller's read-only descriptors showed a
full-speed HID interrupt OUT endpoint 0x01 with bInterval 4 and packet size64.
That descriptor is a USB scheduling fact, not proof of the actuator's achievable
parameter update rate. No output transfer was sent during descriptor inspection.

The source now selects a shared 4 ms target for Switch 2 Pro driver gating and
worker start-to-start pacing. The worker subtracts the synchronous write time
from its remaining wait, rather than appending another4 ms after writing.
Other controller drivers retain their legacy pacing. Matching unsent states for
one device/report are replaced in place, without allocating more queue slots or
incrementing the semaphore. A stop replaces that device's waiting state and
moves it ahead of other waiting requests; an already executing write cannot be
preempted. Existing callbacks are not overwritten.

`SDL_JOYSTICK_HIDAPI_RUMBLE_TRACE=1` enables fast-path diagnostics containing
write start time, age of the latest queued state, write duration, requested size
and actual hid_write return value. Enqueue success alone is not USB delivery.
This patch does not add transport retries or claim successful hardware writes.

The offline specification model covered10000 state replacements, stop priority,
and10000 randomized queue operations. It demonstrates bounded latest-state
storage and the intended timing arithmetic; it does not execute the C worker.
No compilation, DLL deployment, emulator UI action or physical rumble was
performed. The deployed DLL still uses the previous scheduling until this source
is compiled and explicitly deployed. USB write latency, input-report continuity,
queue age, stop latency and physical effects remain hardware acceptance items.
Multiple active controllers share a worker, and a write taking over4 ms cannot
sustain a4 ms start-to-start period.


### 4 ms DLL build and GUI deployment

The 4 ms scheduling source was subsequently compiled successfully on Windows.
The new SDL3.dll SHA-256 is
`1311659FE8C8C903152B050BF81E971F31A6FDC13D7ADAA43D31123368E7C972`.
Copies were deployed to SavageWindRecomp/build and Lichii/build/bin, with the
previous98ED92DDB23E DLL kept as a local backup in each destination.

The134 virtual API checks and the standalone frequency/field codec checks passed.
A harness compiled from the actual rumble worker C source with mocked hid_write
also passed10000-update coalescing and stop-priority/order checks. With no USB
write cost it observed31 start intervals: mean4.422 ms, min4.020 ms, max4.526 ms.
Those values measure the mock worker and OS scheduling, not physical USB output.

The old command-line replay script and Replay-HD1-Effects.cmd launcher have
been removed. The retained interactive player is SavageWindRecomp/tools/
rumble_catalog_player.py, titled "旷野之息 · 原生震动目录与回测". Its extracted catalog
and recorded results remain local ignored data. Physical playback requires the
user to select an effect and press Play.

## 2026-11-06: synchronous-wait decomposition and bounded asynchronous OUT

This experiment uses independently built `test/testswitch2transport.cpp`, not a
replacement runtime DLL. It initializes the attached USB 057e:2069 with the
existing SDL driver, then sends only synthetic, mirrored 160/320 Hz packets:
low amplitude 0.02..0.05 and high amplitude 0.02. Report ID is 2, packet size 64;
Windows HID capabilities confirmed a 64-byte numbered output report. Raw game
resources, firmware and saves are not used.

Four 0.8-second comparisons were captured with UCX and USBXHCI ETW providers:

| Transport | Maximum in flight | Active successful requests | Kernel completion spacing, mean | Submission-to-user-completion, mean |
| --- | ---: | ---: | ---: | ---: |
| Native Windows overlapped HID | 1 | 100 | 8.0006 ms | 7.0005 ms |
| Native Windows overlapped HID | 2 | 200 | 3.9968 ms | 5.0008 ms |
| libusb asynchronous interrupt OUT | 1 | 100 | 8.0001 ms | 7.4900 ms |
| libusb asynchronous interrupt OUT | 2 | 200 | 3.9970 ms | 4.9336 ms |

All requests in these comparisons succeeded with 64 bytes. The tested endpoint
was verified in the kernel rundown as interrupt OUT 0x01, max packet size 64 and
bInterval 4. The analyzer identifies the 057e:2069 device in rundown, filters its
OUT endpoint, pairs dispatch/completion by IRP+URB, and matches these with probe
submission timestamps. It does not include other USB peripherals in statistics.
Raw ETL/XML and local timing logs stay in ignored build output.

For the libusb single-request run, the mean 7.4900 ms divided as follows:

- User submission to UCX dispatch: 0.0056 ms.
- UCX dispatch to kernel completion: 7.4718 ms.
- Kernel completion to libusb callback: 0.0126 ms.

Thus most of the wait is below the user-mode completion loop, not callback
processing. UCX timing includes kernel queueing, host scheduling and device
response; it does not independently separate those or reveal link-level NAKs.
The original Lichii/SDL synchronous path was also replayed under ETW. Its longest
continuous group had 104 OUT requests: mean kernel residence 7.6153 ms and mean
completion spacing 8.0004 ms. This reproduces the original observation without
assuming `hid_write` duration is physical wire transmission time.

With two libusb requests in flight, a further 3-second run completed 750 active
requests without failure or coalescing. Across 749 intervals, kernel completion
spacing averaged 3.99979 ms, min 3.7292, max 4.0121 ms. This establishes approximately
250 successful host USB request completions/second for this setup. It does not
establish 4 ms end-to-end effect latency: an individual request still took about
5 ms, and actuator onset/frequency/force were not measured. The result rules out
an unconditional 125 Hz transport ceiling for this controller/setup.

### Stop, cancellation and connection scope

The experiment keeps at most two fixed buffers in flight. At Stop it ceases
active publication, requests cancellation, drains all callbacks, then submits
one zero-amplitude stereo report. Buffer/OVERLAPPED/transfer storage is retained
until completion even when cancellation races normal completion. There is no
unbounded FIFO and no active submission after Stop. A transport error also causes
publication to cease; after draining, silence is attempted because a timeout is
not proof of disconnect.

Normal stop request-to-successful-completion measurements were 4.97..9.00 ms.
A separate full-pipeline test deliberately submitted two requests immediately
before Stop. Native HID returned two ERROR_OPERATION_ABORTED completions;
libusb returned two LIBUSB_TRANSFER_CANCELLED completions. The subsequent
64-byte zero-amplitude reports succeeded, after 3.491 ms (native) and 3.936 ms
(libusb) from the stop request. No active callback followed the successful stop,
and both tests closed with zero in-flight requests. These are software/transport
stop checks, not human confirmation of actuator silence.

Physical unplug/replug was not performed: the user explicitly said it was not
convenient at this time. Real NO_DEVICE handling, reconnect behaviour and human
stop acceptance therefore remain pending. Cancellation/drain/close success must
not be reported as physical-disconnect verification.

### Reproduction and deployment boundary

Build the Windows-only probe from a Visual Studio x64 Developer PowerShell:

```powershell
cl /nologo /std:c++20 /EHsc /O2 /W4 /I include /I build/deps/libusb-1.0.30/include test/testswitch2transport.cpp /Febuild/rumble-transport-probe/testswitch2transport.exe /Fobuild/rumble-transport-probe/testswitch2transport.obj /link build/windows-x64-release/Release/SDL3.lib build/deps/libusb-1.0.30/VS2022/MS64/dll/libusb-1.0.lib hid.lib setupapi.lib
```

Place the matching SDL3/libusb DLLs beside the probe. `--self-test` checks bounded
packet amplitudes, stereo silence, sequence wrap and Windows report-ID layout
without opening a controller. Physical runs require `native1`, `native2`, `usb1`
or `usb2`, followed by explicit `--play`; optional `--long`, `--cancel-stop` or
`--disconnect` selects a three-second run, full-pipeline stop test or a ten-second
manual-unplug window. Redirect stdout/stderr to local JSONL. On a live transport,
normal and cancellation tests request silence before closing.

`test/analyze_switch2_transport.py` consumes those JSONL files and tracerpt XML.
Pass the XML wall-clock offset explicitly; this host's tracerpt incorrectly
printed a +07:59 suffix for the actual UTC+08 wall clock. The analysis used 480
minutes, validated by sub-millisecond matching against QPC/FILETIME anchors.
Unmatched traces or lost ETW events are rejected rather than silently pooled.

The libusb experiment releases the SDL handles after initialization before
opening its own handle/context: retaining SDL's claimed WinUSB interface prevented
a second composite-device open. A production implementation must instead use
the existing SDL-owned handle/context and integrate completion/cancellation with
its lifetime. It must preserve sensor/input processing and bounded stop ordering.
No application DLL was replaced and neither game was launched for this experiment.
Do not infer that BOTW or Lichii already uses this two-request USB path.


## 2026-11-06: production Windows two-in-flight transport

Windows USB Switch 2 Pro now uses the ordered asynchronous transport in
`src/joystick/hidapi/SDL_switch2_rumble_async.h`, called from the existing
Switch 2 driver. It borrows the existing HIDAPI libusb handle/context and uses
its existing event thread; it neither reopens the device nor replaces a driver.
Only the HID interface associated with that device is used to select its
64-byte interrupt OUT endpoint. Other controllers and other host platforms
retain their existing output paths. Public API signatures and gain are unchanged.

The transport has one submitting thread, two immutable in-flight buffers and
one replaceable pending active state. Actual submissions receive consecutive
64-bit ordinals and firmware sequence values modulo 16. Completed slots retire
strictly in submission order, even when callbacks arrive out of order. An older
failed packet is never retried behind a newer packet: the endpoint is first
cancelled/drained, then only current state may be retried. There is no unbounded
output FIFO. Faster intermediate active changes can still coalesce.

Stop is a latched barrier: it discards unsent old active state, cancels/drains
both in-flight writes, sends zero amplitude on both sides and waits for success
before releasing newer active output. Cancellation racing normal completion is
accepted; buffers and transfers are retained until callback processing finishes.
Short or failed stop writes retry while new active output stays held. Close
pauses the transport and drains it before releasing the borrowed handle. Read
errors, NO_DEVICE callbacks and immediate NO_DEVICE submission failures permanently
reject further output for that device lifetime; reopen cannot replay cached state.
A fresh reconnected device receives a fresh transport. A close-time stop failure
has a one-second acceptance deadline, followed by cancellation/drain before free.
A disconnected controller cannot acknowledge silence; physical motor silence is
not guaranteed by this software mechanism alone.

The driver no longer adds a second millisecond gate in front of this worker.
Actual submissions use a monotonic 4 ms minimum interval and SDL_DelayPrecise;
auto-reset wake events handle idle/callback notification. A timed wait is bounded
by one period, so a newly arriving stop can wait up to 4 ms for this worker before
the cancellation barrier begins. Precise waiting may use short CPU spinning,
as documented for SDL's precise timer API.

Validation of the final build:

- CTest `testswitch2async` executes this exact production header with fake libusb,
  no enumeration or physical output. It passed 10,000 replacements, immutable
  buffers, reversed callbacks/FIFO retirement, both cancellation and delayed
  cancellation, stop-before-new-effect, failed/short stop retry, stale-packet
  suppression, NO_DEVICE from callbacks/submission, input-side disconnect
  notification and fresh-lifetime cleanup. Release assertions remain enabled.
- All 134 virtual haptics API checks and all 1024 frequency/field codec checks
  passed. The existing generic C rumble-worker harness also passed its 10,000
  replacement and stop-order checks, confirming the other-controller queue path.
- The final DLL's public API test sent 502 requests with zero API failures on
  Windows/USB 057e:2069. It covered two seconds of changing stereo output, stop,
  close, silent reopen and close during active output. Gyro and acceleration were
  enabled; 998 combined sensor events arrived during the two-second active phase.
- ETW matched all 498 submissions to the selected controller's kernel OUT requests,
  with zero lost ETW events. Ordinals/firmware sequences were ordered and at most
  two requests were in flight. All successful stop writes followed drainage of
  older callbacks, and no stale active report appeared during reopen.
- In the final public-API run, the main continuous phase had 490 successful active
  requests: 489 kernel completion intervals averaged 4.07785 ms, min 1.9901 ms,
  max 8.0046 ms. An earlier candidate measured 4.02163 ms. Host scheduling and
  completion batching cause jitter; this is approximately 4 ms throughput, not a
  guarantee of an exact 4 ms bus interval, zero loss of intermediate states, or
  4 ms actuator latency. No physical frequency/force calibration was performed.
- Deployed application harnesses also passed: BOTW's actual runtime worker and
  Lichii's actual SDLDriver, each using its adjacent updated DLL. Traces matched
  245 and 203 submissions respectively, verified FIFO/sequence order and final
  silence. Their longest steady completion samples averaged 4.09940 and 4.07566 ms.
- BOTW vibration, input-focus, Betop state/effect and PC-control regression targets
  passed. Its stereo worker test was corrected to wait for observed completion
  and renew the foreground heartbeat instead of assuming an 8 ms sleep suffices.
  Lichii's nine vibration test cases and mailbox regression passed with the new
  DLL. No emulator/game or private save was opened.

Actual physical unplug/replug and subjective stop/feel remain untested because
the user said physical unplug was inconvenient. The NO_DEVICE/disconnect paths
above are simulated regression coverage, not a physical-unplug claim.

Final SDL3.dll SHA-256: `33BCE43468DA37B645EE3CB6FFD64A01BF0A03D31AD74FD7E4686AF539ECC2BD`.
Copies were verified in SavageWindRecomp/build, Lichii/build/bin and
Lichii/build/windows-msvc/bin. Prior DLLs were retained as local
`SDL3-before-async2-<hash-prefix>.dll` backups. libusb-1.0.dll is unchanged.
Local deployment and timing reports live in ignored build/rumble-async-test.
`test/analyze_switch2_async.py` reproduces FIFO, stop-drain and kernel-trace checks.


## 2026-11-06: current event-driven output across all three applications

This supersedes the software-periodic production scheduling above. The physical
controller's full-speed interrupt OUT descriptor advertises bInterval=4; that is
not a requirement to sleep four milliseconds before submitting a fresh value.
New frequency/amplitude states now publish immediately from an application owner
and enter libusb immediately whenever a slot is available. At most two complete
stereo reports are in flight, plus one replaceable pending state. Each report
contains both frequency/amplitude components for both sides: slots are requests,
not actuator channels. Input/expiry polling remains timed; healthy fresh output
has no four-millisecond gate at either application or SDL level.

The Pro driver's public rumble entry points now enqueue asynchronous output
immediately instead of waiting for the next input/device update. Repeated byte-
identical states are deduplicated; unchanged keepalive uses the original driver's
12 ms policy. Retry backoff remains four milliseconds. The serialized submitter,
firmware counters, FIFO retirement, Stop/cancel/drain/zero barrier and NO_DEVICE
handling are unchanged and passed their production-code mock regressions.

The earlier recurring 8 ms gaps had two software contributors. Relative
actual-send+4ms deadlines accumulated wake lateness; SDL_DelayPrecise also always
attempts an OS sleep, even for a tiny final tail, which could add roughly 0.5 ms
on this host. Data was already pending in all ten identified baseline gaps from
the two-second reference. Removing the healthy software gate avoids both waits,
rather than trying to maintain a second clock in front of the USB host scheduler.
A residual during an intermediate fixed-phase experiment was separately traced
to a publisher delivering new data about 0.333 ms late; SDL forwarded it within
about 1 microsecond. An asynchronous transport cannot transmit a not-yet-produced
value. A legitimate 5 ms input can naturally occupy 4/8 ms USB service opportunities.

Application changes:

- BOTW: publication wakes its existing owner, with no healthy pacing gate. The
  input/lease timeout and 100 ms foreground-heartbeat protection remain. A fresh
  stop bypasses old active-failure backoff. The instant-backend regression forwarded
  1 ms publications with measured mean 1.505 ms, proving no forced 4 ms delay.
- Lichii: publication signals a coalesced semaphore notification; its owner wakes
  immediately. The 4 ms idle timeout is only input/expiry service. Mailbox timing
  limits now apply only to failed submissions; latest state, side-specific stops,
  connection generations and 250/2000 ms renewal/lease behaviour remain.
- TOTK: removed the 10 ms guest-side throttle and synchronous SDL call. A dedicated
  owner copies raw stereo state under the guest mailbox lock, releases it before
  device I/O, and wakes on scalar/batch publication. Batches publish atomically.
  It preserves one-time strength, raw readback, independent 250 ms side expiry,
  permission/focus/config stops, device epochs and bounded retry. Normal process
  exit paths explicitly join/drain it because Windows plat_exit skips atexit.
  The v143 executable and matching dynamic libusb dependency were rebuilt/deployed.

Final event-driven hardware records (Windows USB 057e:2069, synthetic low-amplitude
input, no game scene or private save):

| Run | Input | Successful main-effect requests | Mean kernel completion gap | Maximum |
| --- | --- | ---: | ---: | ---: |
| Public SDL API | 500 changing updates/2 s, 4 ms | 500 | 3.99585 ms | 4.1276 ms |
| BOTW owner | 250 changing updates/1 s, 4 ms | 250 | 3.99832 ms | 4.1189 ms |
| Lichii owner | changing 4 ms input; startup excluded | 211 | 3.98876 ms | 4.0971 ms |
| TOTK HLE + real input backend | 200 updates/1 s, 5 ms | 200 | 4.98508 ms | 8.0023 ms |

All traces passed one-to-one user/kernel request matching, consecutive ordinals,
modulo16 firmware sequences, at-most-two in flight, drain-before-stop and final
silence. The 4 ms input runs had no 8 ms interval in these samples. TOTK's 4/8 ms
gaps are expected for its deliberately 5 ms stimulus, not the removed 10 ms throttle.
Kernel completion timing is not a link-layer ACK or actuator-onset measurement.
Short tests do not guarantee hard real-time scheduling under all CPU/USB loads.

Regression: SDL production transport CTest, 134 virtual haptics API checks and 1024
codec values; BOTW vibration/focus/Betop/PC-control targets; Lichii's nine vibration
cases and mailbox check; TOTK owner stall / 10,000 publications, retry, raw strength
readback, stop/permission, per-side expiry, epochs, join tests and v143 --selftest
all passed. Actual unplug/replug remains deferred at the user's request.

Current DLL SHA-256: `8F40DCD19C88785EA07141AA28494DF14CB8B78FB3B0951B915B3EC07ECC32FF`. Copies match in BOTW/build, TOTK/build/v143 and both
Lichii Windows output directories. No old DLL backups were recreated.

## 2026-11-06: measured queue-depth selection

The subsequent [one-through-four-slot comparison](README-switch2-queue-depth.md)
keeps the default at two. Across three interleaved rounds, depths 2/3/4 each
completed all 2250 changing 4 ms states and 1800 changing 5 ms states. The 4 ms
segments had no approximately 8 ms completion gaps; 5 ms segments naturally had
4/8 ms spacing. Neither workload ever occupied a third slot, even with a limit
of 3 or 4. One slot merged 1122/2250 and 674/1800 source states respectively.
Short-pulse and stop measurements, limitations, reproducible tooling and
regressions are in the linked report. The comparison used an isolated diagnostic
DLL; application DLLs retain their already-deployed two-slot default.

## 2026-11-06: raw input rate after motherboard USB reconnection

The [raw input capture report](README-switch2-input-rate.md) compares SDL input,
native Windows HID and report formats 0x05/0x09, including the user's motherboard
USB reconnection. All five captures show approximately 250 physical IN reports/s
with 4.000 ms kernel completion spacing. In the final six-second normal window,
1500 raw reports and 1500 physical IN completions produce 1500 gyro plus 1500
accelerometer events; adding the two event types yields 500 events/s, not 500
packets/s. Public Steam 500 Hz accounts and Switch2Connect's wired reader were
investigated, but no raw-packet-verified 500 Hz procedure was reproduced. Normal
0x05 reporting was restored after the raw-only 0x09 diagnostic contrast.

## 2026-11-06: production sender finalization

The selected two-request event-driven transport is deployed to BOTW/build,
TOTK/build/v143 and both Lichii output directories with matching dynamic libusb.
The SDK hash above is now the production-final build. Diagnostic publication IDs
and timestamps are retained behind the existing disabled-by-default trace hint.
The private 1..4 queue-depth override remains diagnostic; normal output uses two.

Retry waiting now uses an interruptible native event timeout rather than precise
sleep/spin. A fresh Stop, disconnect or close can wake the owner during failed-I/O
backoff. A production-code regression inserts a one-second retry deadline and
requires Stop to submit silence within 200 ms instead of waiting for that deadline.
Normal changed output still has no software pacing gate. Ordered immutable
buffers, FIFO retirement, stop/drain/zero barriers and device lifetime protection
remain unchanged.

On the final DLL, 750/750 changing 4 ms and 600/600 changing 5 ms updates completed;
the 4 ms segment had no approximately 8 ms gaps. All 12 burst stops succeeded
with the old callbacks drained first. Independent real-USB BOTW, Lichii and TOTK
sender fixtures also passed ETW ordering and final-zero checks. Largest steady
segments had completion gaps averaging 3.991/3.999/4.985 ms respectively, maximum
4.012/4.025/8.008 ms (TOTK used a 5 ms source). Captures are ignored local artifacts
under build/rumble-production-final, including deployment.json and per-run
validation JSON. No actual game scene or physical unplug acceptance is claimed.

Offline regression passed: production transport CTest including all four depth
bounds and Stop interruption, 134 virtual API checks, 1024 codec values, BOTW
vibration/worker/focus targets, Lichii nine vibration cases plus mailbox, and TOTK
owner blocking/retry/permission/expiry/epoch/shutdown cases. Old DLL backup files
were not recreated. Short-pulse limitations from the queue-depth report remain;
no unrequested pulse stretching was introduced.
