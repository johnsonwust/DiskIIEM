# DiskIIEM · Pico 2 固件

[English](README.md)

DiskIIEM 是一款面向带 Disk II 控制卡的 Apple II 系统的 5.25 英寸软驱仿真器。固件运行在 Raspberry Pi Pico 2（RP2350）上，把 SD 卡里的磁盘镜像作为软盘提供给 Apple II。用户可以在机身 LCD 上用四个按键浏览、装载和切换镜像。

本仓库提供 RP2350 固件源码、引脚定义和构建脚本。源码标注的固件版本为 **v1.2.1**。 [版本定义](Branding.h)

## 规格

| 项目 | 规格 |
| --- | --- |
| 主机接口 | Apple II 5.25 英寸 Disk II 驱动器接口。包含四相步进、驱动器选择、写使能、写数据输入、读数据输出及写保护输出 |
| 主控 | Raspberry Pi Pico 2（RP2350）。CMake 构建目标为 `pico2` 和 `rp2350-arm-s` |
| 时钟和运行方式 | 固件启动时将系统时钟设为 200 MHz，内部稳压器设为 1.20 V，程序复制到 RAM 中运行 |
| 镜像格式 | `.dsk`（DOS 扇区顺序）、`.nic`、`.woz`（5.25 英寸 WOZ 1 和 WOZ 2） |
| SD 卡 | FAT32，使用 SPI0。初始化时钟为 400 kHz，固件请求的工作时钟为 10 MHz |
| 显示 | 128 × 64 单色 ST7567 LCD，SPI1 初始化频率为 500 kHz。背光低电平点亮，有卡时闲置 15 秒后自动熄灭 |
| 按键 | 上、下、返回、确认四个按键，低电平有效。长按上、下键可快速滚动 |
| 声音 | PWM 蜂鸣器提供寻道和按键提示音，固件配置为 2 kHz，10% 占空比 |
| 诊断 | LCD 可查看卡和镜像状态。设置菜单可将写盘日志和原始磁道数据导出到 SD 卡 |

硬件规格依据 [PinConfig.h](PinConfig.h) 记录的接线。构建目标和时钟设置分别见 [CMakeLists.txt](CMakeLists.txt) 和 [DiskIIEM.cpp](DiskIIEM.cpp)。

## 磁盘镜像支持

| 格式 | 文件布局 | 仿真与写回 |
| --- | --- | --- |
| DSK | 143,360 字节（140 KiB）。35 道 × 16 扇区 × 256 字节，按 DOS 顺序排列 | 读取时由扇区数据生成 GCR 磁道，写入时解码并保存扇区数据。采用标准 16 扇区布局 |
| NIC | 286,720 字节（280 KiB）。35 道，每道 16 个 512 字节槽，每槽含 416 个 GCR 磁道字节 | 播放时去除槽内填充，写入时解码并按槽布局保存。采用标准 16 扇区布局 |
| WOZ 1 / WOZ 2 | 磁道位流和四分之一轨映射，磁道长度和位时间可变化 | 支持普通位流磁道读写，受写保护等条件限制。WOZ 2 的 FLUX 磁道可回放且为只读。较大的 WOZ 2 文件按轨从 SD 卡读取，WOZ 1 须装入镜像缓冲区 |

文件浏览器显示这三种扩展名。固件支持 5.25 英寸 Disk II 接口，DSK 使用 DOS 扇区顺序。DSK 和 NIC 的写回以标准扇区为单位。需要保留非标准磁道布局或位流的镜像可使用 WOZ。带保护的磁盘能否运行取决于具体的磁道布局和时序。

此固件的 DSK 和 NIC 磁道长度为 53,248 位，标称位时间 4 µs，对应约 213 ms 每圈（282 rpm）。测量转速的软件可能显示低于 300 rpm 的数值。 [磁道常量](FloppyEmulator.h) · [设计和兼容性记录](WOZ_DESIGN.md)

## 实现原理

### 实时时序和双核分工

- **核心 0** 处理 Disk II 接口，跟踪磁头位置、准备磁道数据、捕获写入并安排待写数据保存到 SD 卡。
- **核心 1** 处理 LCD、按键和文件浏览，以及从菜单发起的装载镜像、格式化等 SD 卡操作。
- **PIO 与 DMA** 负责连续输出读盘信号。PIO 状态机按位定时输出，两路链式 DMA 交替从两个 64 字节缓冲区供数，中断补充空闲的缓冲区。

### 读盘和寻道

驱动器被选中时，控制卡的四相步进信号决定虚拟磁头位置。对于 DSK，固件生成 GCR 磁道，地址域使用 4-and-4 编码，数据域使用 6-and-2 编码。对于 NIC，固件解包现有磁道数据。对于 WOZ，固件按镜像的四分之一轨映射选取磁道。在默认 4 µs 位单元下，`RD_DATA` 用 1 µs 高脉冲表示 `1`，用整个单元保持低电平表示 `0`。WOZ 播放采用镜像中的位时间，切换长短不同的磁道时保持旋转角度对应。设置菜单也可开启弱位模拟。

### 写盘和保存

`DRIVE_SEL#` 和 `WRITE_ENABLE#` 都有效时，固件暂停读信号，用持续运行的 40 MHz 定时器记录 `WR_DATA` 的翻转，计时分辨率为 25 ns。固件根据翻转间隔还原数据位，并把它们合并到当前磁道。300 µs 的等待窗口会把紧邻的写入段归入同一会话，以支持 DOS 3.3 分段写入扇区地址域和数据域的操作。

更新后的磁道先提交到内存中的镜像。DSK 和 NIC 的写入解码后按扇区保存。WOZ 的写入直接更新已有的位流磁道，保留原磁道布局。装载 WOZ 时会检查磁道边界和非零 CRC，首次写入磁道前会清零该 CRC 字段。

待保存数据通常在驱动器取消选中后写回 SD 卡。失败的写入保留待重试。关闭 Apple II 电源或拔卡前，等 `UNSAVED` 提示消失。

[仿真与写回代码](FloppyEmulator.cpp) · [PIO 程序](floppy_bit_output.pio) · [WOZ 解析](WozImage.cpp)

## 硬件引脚

表中编号为 **Pico GPIO 编号**。Disk II 连接器接线和供电应以所用板子的原理图为准。当前板子的七路 Disk II 输入通过串联二极管接入，Pico 侧以 2.2 kΩ 电阻上拉至 3.3 V。`RD_DATA` 与 `WR_PROTECT` 经过同相 74HC125 缓冲器，其输出使能由驱动器选择信号控制，未选中时释放共享线路。早期演示板使用反相输出，须将 `READ_OUTPUT_INVERTED` 和 `WPROT_OUTPUT_INVERTED` 设为 `1`。

| 信号或用途 | Pico GPIO | 方向和行为 |
| --- | --- | --- |
| `PH0`、`PH1`、`PH2`、`PH3` | GP2、GP3、GP4、GP5 | 输入，高电平有效 |
| `RD_DATA` | GP10 | 输出，空闲为低，`1` 为高脉冲 |
| `WR_DATA` | GP11 | 输入，通过电平翻转传递写入数据 |
| `WRITE_ENABLE#` | GP12 | 输入，低电平有效 |
| `DRIVE_SEL#` | GP13 | 输入，低电平有效 |
| `WR_PROTECT` | GP14 | 输出，高电平表示写保护 |
| SD 卡检测 | GP15 | 输入，低电平有效 |
| SD SPI0 | GP16 MISO、GP17 CS、GP18 SCK、GP19 MOSI | SD 卡数据与控制 |
| LCD SPI1 | GP26 SCK、GP27 MOSI、GP9 CS | 显示数据与控制 |
| LCD A0、复位、背光 | GP20、GP22、GP21 | 输出，背光低电平点亮 |
| 下、上、返回、确认 | GP6、GP7、GP8、GP28 | 按键输入，低电平有效 |
| 蜂鸣器 | GP0 | PWM 输出，静音时为低电平 |

[引脚表和板级说明](PinConfig.h)

## 安装和操作

1. 准备一张 FAT32 格式的 SD 卡，先复制一个标准 140 KiB、DOS 顺序的 `.dsk` 镜像。其他镜像可以按文件夹归类。
2. 关闭 Apple II 电源，将已装配好的 DiskIIEM 接到 Disk II 控制卡，插入 SD 卡，再打开 Apple II 电源。
3. 进入 **Files**，用上、下键选择镜像，再按确认键装载。返回键可回到上一级目录或主页。
4. 从已装载的镜像启动，或在 Apple II 上像使用软盘一样访问。写盘后等待未保存提示消失。

| 菜单 | 用途 |
| --- | --- |
| **Files** | 浏览子目录、装载和切换镜像。目录排在文件前，每屏显示六项，浏览时无需将整个目录列表加载到内存 |
| **Status** | 查看卡类型、SPI 速度、卷容量、分区起点和簇大小 |
| **Info** | 查看镜像类型、当前磁道、写保护和保存状态。WOZ 还可显示四分之一轨位置和位时间 |
| **Config** | 设置声音、自动装载、弱位模拟及当前镜像的写保护，或执行格式化和导出诊断 |

设置及上次装载的镜像保存在每张卡根目录的 `config.cfg` 中。写保护随镜像文件的 FAT32 只读属性保存。WOZ 内部的写保护独立生效。**Format SD** 会清除卡上的文件并重建一个 FAT32 分区，用前请先备份。

出现 `NO SD CARD` 时，可重新插好卡并等设备重试。出现 `exFAT detected!` 时，先备份文件并将卡格式化为 FAT32。遇到可复现的读写故障，可用 **Config → Save debug log** 导出 `WLOGnn.TXT`（信号电平和最近的写盘事件）和 `WTRKnn.BIN`（当前磁道数据）。 [菜单参考](UI_MENU.txt)

## 构建和烧录

项目使用 CMake、Raspberry Pi Pico SDK **2.2.0** 和 `arm-none-eabi-gcc`。Windows 下还需安装 Git 和带 make 工具的 MinGW-w64。进入仓库根目录后，在 PowerShell 中运行：

```powershell
.\build.ps1
```

脚本会在缺少 Pico SDK 和 picotool 2.2.0 时获取它们，并生成 `build/DiskIIEM.uf2`。如果脚本找不到 ARM 编译器，可将 `PICO_TOOLCHAIN_PATH` 设为工具链根目录。烧录时按住 Pico 2 的 **BOOTSEL** 键接入 USB，把 UF2 复制到出现的磁盘。 [构建脚本](build.ps1) · [CMake 配置](CMakeLists.txt)

## 仓库内容

| 文件 | 用途 |
| --- | --- |
| [DiskIIEM.cpp](DiskIIEM.cpp) | 启动、双核配置和主循环 |
| [FloppyEmulator.cpp](FloppyEmulator.cpp)、[DiskCodec.cpp](DiskCodec.cpp)、[floppy_bit_output.pio](floppy_bit_output.pio) | 磁道输出、GCR 编解码、写入捕获及写回 |
| [WozImage.cpp](WozImage.cpp) | WOZ 文件头、磁道映射、FLUX 元数据、CRC 校验 |
| [SDCardManager.cpp](SDCardManager.cpp)、[FAT32.cpp](FAT32.cpp)、[SDFormat.cpp](SDFormat.cpp) | SD 卡读写、FAT32 文件操作及格式化 |
| [UIHandler.cpp](UIHandler.cpp)、[UIKeys.cpp](UIKeys.cpp)、[UIRender.cpp](UIRender.cpp) | 菜单、按键操作及 LCD 绘制 |
| [PinConfig.h](PinConfig.h)、[Branding.h](Branding.h) | 引脚定义、板级选项及固件版本 |
| [DEVELOPMENT.md](DEVELOPMENT.md)、[WOZ_DESIGN.md](WOZ_DESIGN.md) | 开发背景、实现历史及验证记录 |

## 验证状态

- **Apple II 实机**。开发记录确认过原型机在 DOS 3.3 上的 `CATALOG`、`SAVE` 和 `LOAD` 操作。
- **主机仿真**。历史检查覆盖扇区读写、整轨格式化、WOZ 解析与回放、FAT32 操作及菜单。物理 PIO 波形和不同 SD 卡、控制卡组合仍需实机覆盖。

实物照片和界面截图将在后续补充。 [验证记录](WOZ_DESIGN.md)
