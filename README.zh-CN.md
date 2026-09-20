# DotMic

[English](README.md)

![DotMic 界面](docs/preview/overview.png)

S3AI Game（ESP32-S3）掌机上的按住说话 USB 麦克风 + 点阵时钟。按住 AI 键说话，
松开静音。说话时屏幕显示真实 FFT 频谱，不说话时显示 NTP 校准的时钟。

设备在电脑上枚举为标准 USB 音频输入，任何录音或听写软件都能直接用。固件只负责
音频输入，语音转文字在电脑上完成。

## 界面

| | |
|---|---|
| ![时钟](docs/preview/01-clock.png) | ![频谱](docs/preview/02-spectrum.png) |
| 时钟页 | 语音页 |

![频谱动画](docs/preview/spectrum.gif)

预览图由 `preview.py` 用固件自己的字库和示例数据渲染，是布局检查，不是实机截图。

## 按键

| 操作 | 按键 |
|---|---|
| 按住说话，松开静音 | AI / GPIO0 |
| 浅睡眠 / 唤醒 | APP / GPIO10 |
| 麦克风增益，1–16，默认 4 | 上 / 下 |
| 打开 / 关闭诊断页 | SELECT |
| 诊断页翻页（STATUS / DEVICE） | 左 / 右 |
| 开关 Wi-Fi 配网 | START |

无操作 10 秒自动休眠。录音中、电脑正在取流、诊断页打开时暂停倒计时。

## 安装

镜像是**纯应用镜像**，用 Launcher 的 TF 卡文件浏览器安装。不能写到 Flash `0x0`，
也不要安装 `.pio` 里的合并镜像或分区表。

1. 把 `dist/DotMic-v1.8.bin` 复制到 TF 卡任意位置。
2. 在 Launcher 文件浏览器里选中它。
3. 用能传数据的 USB 线连电脑。应用启动后 USB 从 Serial/JTAG 切换为音频 + CDC，
   COM 号可能变化。
4. 在录音设备里选 `S3AI DotMic`，部分驱动显示为 `TinyUSB UAC1`。
5. 按住 AI 说话。

USB 为 UAC1 单声道 48 kHz / 16 bit，附带 CDC 串口用于日志。ESP32-S3 只有 BLE，
不支持经典蓝牙 HFP，所以不能作为普通蓝牙耳机麦克风配对电脑。

USB 无响应时，按住 AI/BOOT 再按重启进入 ROM 下载模式。正常启动时不要按住 AI。

## 时钟校准

时间来自 Wi-Fi + NTP。按 START 进配网，手机连 `samestick` 热点填好网络，设备会自动
连线并同步 `ntp.aliyun.com`、`time.cloudflare.com`、`pool.ntp.org`，固定显示北京时间。

同步后断开电脑仍继续走时，浅睡眠期间 RTC 也在跑。完全断电后时间无效，屏幕显示
`--` 和 `START: WIFI`。

## 频谱

屏幕上是真实 FFT，不是按音量驱动的动画：

- 48 kHz PCM，1024 点 Hann 窗，频率分辨率 46.875 Hz，单帧约 21.3 ms
- 24 个分析频段，近似对数划分，覆盖约 94 Hz–12 kHz
- 插值到 32 个显示列，多出来的列是平滑，不是新增频率分辨率
- 固定 −66 至 −6 dBFS 高度映射，不做自动增益，所以安静环境屏幕就是安静的
- 60 ms 上升、180 ms 回落，五点加权平滑
- 低频橙红，中高频灰白

平滑只影响显示，传给电脑的录音不变。它显示的是各频段能量，不是音高或音符识别，
一个声音通常会因为谐波同时点亮好几个频段。

`USB IDLE` 表示电脑没打开音频流，`USB LIVE` 表示已打开。没按 AI 时 USB 连接保持，
但提交零样本。设备本地不录音、不上传。

## 硬件

| | |
|---|---|
| 主控 | ESP32-S3，16 MB Flash，8 MB PSRAM |
| 屏幕 | 240x240 ST7789，SPI mode 0，40 MHz，X 镜像，BGR 子像素 |
| 麦克风 | MSM261S I2S，BCLK 4，WS 5，DIN 6 |
| 屏幕引脚 | MOSI 46，SCLK 11，DC 12，CS 3，RST 7，背光 9 |
| TF 卡 | 1-bit SDMMC，CLK 40，CMD 39，D0 41 |
| USB | 原生 GPIO19/20，VID `303A`，PID `D07C` |

麦克风工作在 48 kHz 双槽 32-bit。启动时从左槽开始，持续比较左右槽 RMS 选择有声音
的那一侧，取高 16 位，去直流并限幅。当前活动槽位在时钟页显示为 `AL` / `AR`，
诊断页直接给出 `rmsL` / `rmsR`。

USB 序列号按 MAC 生成（`DOTMIC-...`），避免 Windows 复用旧的音频实例。

每次开机检查 TF 卡：创建唯一的 `/dotmic-check-xxxxxxxx.tmp`，写入、读回比对、删除。
失败只报告，不格式化。

## 构建

```bash
pio run -e s3ai-dotmic
python package.py
```

`pio run` 只生成 `.pio/build/s3ai-dotmic/firmware.bin`，`package.py` 才会校验镜像和
分区边界并写入 `dist/`。固件和 `package.py` 的版本号必须一致，否则打包直接报错退出。

依赖全部从 PlatformIO registry 解析，板级定义已放进 `boards/`
（见 [boards/NOTICE.md](boards/NOTICE.md)），干净 clone 不需要任何其他仓库就能构建。

Windows 上首次构建前要先启用长路径，否则工具链解压会失败：

```powershell
New-ItemProperty -Path "HKLM:\SYSTEM\CurrentControlSet\Control\FileSystem" `
  -Name LongPathsEnabled -Value 1 -PropertyType DWORD -Force
```

要在 PowerShell 或 cmd 里跑 PlatformIO，不要用 Git Bash / MSYS。平台用
`idf_tools.py` 安装编译器，该脚本拒绝在 MSys/Mingw 下运行，结果是编译时报
`xtensa-esp32s3-elf-g++` 找不到。

重新生成界面预览图（需要先跑一次 `pio run` 把字库拉下来）：

```bash
python preview.py
```

频谱映射有一份主机侧测试，直接编译固件用的同一个 `src/spectrum.h`：

```bash
zig c++ -std=c++17 -O2 tools/test_spectrum.cpp -o test_spectrum
./test_spectrum
```

覆盖 234.4 / 1875 / 9375 Hz 频段映射、双音混合、静音、直流去除、6 dB 幅度变化、
噪声门限、显示范围、不同帧间隔的一致性和缓降到静音。这些是算法测试，不代表录音
质量、USB 时序或实机性能已经验证。

## 当前状态

已编译、打包并生成校验和，见 `dist/manifest.json` 和 `SHA256SUMS.txt`。音质、显示、
唤醒行为和休眠电流**尚未在实机上测量**。上面这些数字是设计目标，不是实测结果。

## 相关项目

- [wifi-portal](https://github.com/sameclub/wifi-portal) — 共享配网库
- [PipBoy](https://github.com/sameclub/pipboy) — 同一台设备上的 Fallout 风格系统监视器
- [VoxStick](https://github.com/sameclub/voxstick) — 本地 coding agent 的桌面终端

## 许可

MIT，见 [LICENSE](LICENSE)。
