# DiskIIEM：5.25 寸软驱仿真器（RP2350）

用 Raspberry Pi Pico 2（RP2350）代替一台 Disk II 5.25 寸软驱：把 SD 卡上的磁盘镜像文件当作软盘，接在 Disk II 控制卡的驱动器接口上使用。

屏幕上显示的产品名、副标题和版本号在 `Branding.h` 里定义。

## 功能

- 镜像格式：`.dsk`（DOS 顺序，140 KB）、`.nic`（280 KB）、`.woz`（WOZ1 / WOZ2，含四分之一轨、不等长磁道、FLUX 磁道）
- 读和写：写入的数据先存入内存中的镜像，驱动器空闲时再写回 SD 卡上的原文件
- SD 卡：FAT32，支持长文件名和子目录，可热插拔；可在本机格式化
- 128×64 单色 LCD（ST7567）和 4 个按键：浏览目录、选择镜像、查看卡和镜像信息、设置
- 写保护：跟随镜像文件，保存在文件的"只读"属性里
- 开机自动加载上次使用的镜像
- 寻道提示音

界面每一页的内容和按键操作见 [UI_MENU.txt](UI_MENU.txt)，各页截图见 [docs/ui_preview.png](docs/ui_preview.png)。

## 硬件

RP2350 以 200 MHz 运行，程序整体复制到内存中执行。两个核心分工：核心 0 负责磁盘信号（读数据输出、写数据捕获、步进），核心 1 负责界面和由菜单发起的 SD 卡操作。

引脚在 `PinConfig.h` 中定义：

| 信号 | GPIO | 方向 | 说明 |
|---|---|---|---|
| PH0–PH3 | 2–5 | 输入 | 步进相位，高有效 |
| RD_DATA | 10 | 输出 | 读数据；空闲为低，数据 1 是 1 µs 的高脉冲 |
| WR_DATA | 11 | 输入 | 写数据，只看电平翻转 |
| WRITE_ENABLE# | 12 | 输入 | 低有效 |
| DRIVE_SEL# | 13 | 输入 | 低有效 |
| WR_PROTECT | 14 | 输出 | 高 = 写保护 |
| 蜂鸣器 BEEP | 0 | 输出 | 高 = 线圈通电，10% 占空比的 PWM，不响时为低 |
| SD 卡检测 | 15 | 输入 | 低 = 有卡 |
| SD MISO / CS / SCK / MOSI | 16 / 17 / 18 / 19 | | SPI0 |
| LCD SCK / MOSI | 26 / 27 | | SPI1 |
| LCD CS / A0 / RST | 9 / 20 / 22 | 输出 | |
| LCD 背光 | 21 | 输出 | 低 = 亮，不用 PWM |
| 按键 DW / UP / RET / ENTR | 6 / 7 / 8 / 28 | 输入 | 低有效 |

以上都是 Pico 引脚上的电平，与接口上的电平相同，板上没有反相器：

- 输入（PH0–PH3、WR_DATA、WRITE_ENABLE#、DRIVE_SEL#）：每路串一个二极管（D1–D7），Pico 一侧用 2.2 kΩ 上拉到 3.3 V（RN1、RN2）。线路断开时读到高电平，固件的内部上拉与它方向一致。
- 输出（RD_DATA、WR_PROTECT、蜂鸣器）：经过 74HC125（U2，不反相、三态）。RD_DATA 和 WR_PROTECT 的输出使能（U2 第 10、13 脚，低有效）接在 `DRIVE_SEL_3V` 上（与 GP13 同一网络，2026-11-25 导出文件里的旧名 `DEVICE_ENABLE_3V` 已合并进来），未选中时释放总线，固件不用控制。

旧的 demo 板在 RD_DATA 和 WR_PROTECT 上各有一个反相缓冲器，用它时把 `PinConfig.h` 中的 `READ_OUTPUT_INVERTED` 和 `WPROT_OUTPUT_INVERTED` 改为 1。

蜂鸣器引脚不能长时间为高（驱动管会发烫）。固件不用软件翻转这个引脚，而是接到一个一直运行的硬件 PWM 上，所以即使程序停住，引脚上也只会是低电平或 10% 的脉冲。

## 源文件

| 文件 | 内容 |
|---|---|
| `DiskIIEM.cpp` | `main()`：启动顺序、两个核心的主循环 |
| `FloppyEmulator.cpp/.h` | 读数据流（PIO + DMA）、写捕获、步进、磁道缓存、写回镜像 |
| `DiskCodec.cpp/.h` | 6-and-2 / 4-and-4 编码和扇区布局 |
| `WozImage.cpp/.h` | WOZ 文件解析 |
| `SDCardManager.cpp/.h` | SD 卡 SPI 驱动 |
| `FAT32.cpp/.h`、`SDFormat.cpp/.h` | FAT32 读写、格式化 |
| `ConfigManager.cpp/.h` | `config.cfg` |
| `UIKeys.cpp`、`UIHandler.cpp/.h`、`UIRender.cpp` | 按键、流程、画面 |
| `Display.h`、`ST7567.cpp/.h`、`Buttons.cpp/.h`、`Beeper.cpp/.h` | LCD、按键、蜂鸣器 |
| `floppy_bit_output.pio` | 读数据位输出 |
| `PinConfig.h`、`Branding.h`、`DebugOut.h` | 引脚、产品名、调试输出开关 |

设计说明、各阶段的改动和已知限制记录在 [WOZ_DESIGN.md](WOZ_DESIGN.md)。

## 编译

需要 pico-sdk 2.2.0、arm-none-eabi-gcc、CMake、Ninja 或 Make、picotool、pioasm。

```
cmake -G Ninja -S . -B build -DCMAKE_BUILD_TYPE=Release -DPICO_BOARD=pico2 -DPICO_PLATFORM=rp2350-arm-s -DPICO_SDK_PATH=<pico-sdk 的路径>
cmake --build build
```

生成 `build/DiskIIEM.uf2`。按住 BOOTSEL 把 Pico 2 接到电脑，把这个文件复制到出现的盘里。

Windows 下也可以用 `build.ps1`（或 `build.bat`），它会检查工具并处理几个常见的环境问题。

## 在电脑上检查和测试

`tools/hosttest` 不需要硬件：它用 clang 按 RP2350 目标编译全部源文件，并把固件的真实源码和虚拟的 SD 卡、磁盘控制器、LCD 一起在电脑上运行，自动检查读盘、写盘、格式化、换卡和菜单操作。说明见 [tools/hosttest/README.md](tools/hosttest/README.md)。

它不能代替上机测试：真实的信号时序、SD 卡的实际行为和接线不在检查范围内。上机测试的步骤见 [test_images/硬件测试方案.txt](test_images/硬件测试方案.txt)。

## 调试

设置页的 Save debug log 会在 SD 卡根目录生成 `WLOGnn.TXT`（写入各环节的计数、引脚电平、最近 256 条写盘事件）和 `WTRKnn.BIN`（当前磁道的原始位）。

## 来源和许可

本项目在一份面向 Pico 2 的软驱仿真固件的基础上修改而成，该固件又参考了 vibr77 的 AppleIIDiskIIStm32F411 项目。后者声明仅限非商业使用并需署名，商业权利由其作者保留。

目前仍有一部分源代码沿用自最初的版本（主要是 SD 卡驱动、FAT32 读取、LCD 驱动和主程序框架），各文件的比例和处理情况见 [WOZ_DESIGN.md](WOZ_DESIGN.md) 的"代码来源"一节。本项目自己的许可证尚未确定。

"Apple"、"Apple II"、"Disk II" 是 Apple Inc. 的商标，这里只用来说明兼容的设备。
