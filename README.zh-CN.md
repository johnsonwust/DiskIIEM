# DiskIIEM · Pico 2 固件

[English](README.md)

**固件源码与构建脚本** · Raspberry Pi Pico 2（RP2350）

DiskIIEM 是一款基于 Raspberry Pi Pico 2（RP2350）的 Apple II 5.25 英寸软驱仿真器。它接入 Disk II 控制卡的驱动器接口，把 SD 卡上的磁盘镜像当作软盘使用。通过机身上的 LCD 和四个按键，可以浏览目录、装载镜像和切换磁盘。

## 主要功能

- 支持 `.dsk`、`.nic` 和 `.woz`（WOZ 1 / WOZ 2）镜像。
- 支持磁盘读写，写入内容会保存回 SD 卡上的镜像文件。WOZ 的 FLUX 磁道为只读。
- 支持 FAT32 SD 卡、子目录和长文件名，可在开机时恢复上次加载的镜像。
- 可从机身菜单查看镜像信息和设置写保护。

## 控制方案

固件利用 RP2350 的双核与 PIO、DMA。一个核心处理 Disk II 的读写信号和步进，另一个处理 LCD 菜单和菜单发起的 SD 卡操作。硬件采用 Pico 2、SD 卡和 128 × 64 单色 LCD，便于控制整机成本。

## 快速开始

1. 将磁盘镜像复制到 FAT32 格式的 SD 卡。
2. 关闭 Apple II 电源，将设备接到 Disk II 控制卡，插入 SD 卡后再开机。
3. 用机身按键选择镜像，然后在 Apple II 上按平常使用软盘的方式访问。

固件采用 Raspberry Pi Pico SDK 2.2.0 构建。Windows 下可运行项目里的 `build.ps1` 生成 `DiskIIEM.uf2`，再通过 Pico 2 的 BOOTSEL 模式写入。引脚定义和菜单操作见 [PinConfig.h](PinConfig.h) 与 [UI_MENU.txt](UI_MENU.txt)。

## 当前状态

原型已完成基础测试，开发记录确认了 Apple II 实机上的 DOS 3.3 目录读取、文件保存和重新加载。实物照片和界面截图将在后续补充。
