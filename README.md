# DiskIIEM · Pico 2 Firmware

[简体中文](README.zh-CN.md)

**Firmware source and build scripts** for Raspberry Pi Pico 2 (RP2350).

DiskIIEM emulates a 5.25-inch floppy drive for the Apple II. It connects to the drive port on a Disk II controller card and uses disk images stored on an SD card. Use the built-in LCD and four buttons to browse folders, mount images, and swap disks.

## Features

- Supports `.dsk`, `.nic`, and `.woz` (WOZ 1 and WOZ 2) disk images.
- Reads and writes disk images, saving changes back to the SD card. FLUX tracks in WOZ images are read-only.
- Supports FAT32 SD cards, subdirectories, and long filenames. It can remount the last-used image at startup.
- Shows disk image details and lets you set write protection from the device menu.

## Hardware and firmware

The firmware uses both RP2350 cores, programmable I/O (PIO), and DMA. One core handles Disk II read, write, and step signals. The other runs the LCD menus and handles SD card operations initiated from those menus. The hardware uses a Pico 2, an SD card, and a 128 × 64 monochrome LCD to keep costs down.

## Quick start

1. Copy your disk images to a FAT32-formatted SD card.
2. With the Apple II powered off, connect DiskIIEM to the Disk II controller card and insert the SD card. Then turn on the Apple II.
3. Select an image with the device buttons and use it on the Apple II as you would a floppy disk.

The firmware builds with Raspberry Pi Pico SDK 2.2.0. On Windows, run `build.ps1` to generate `DiskIIEM.uf2`, then copy it to the Pico 2 drive in BOOTSEL mode. See [PinConfig.h](PinConfig.h) for pin assignments and [UI_MENU.txt](UI_MENU.txt) for menu controls.

## Project status

Basic tests on an Apple II running DOS 3.3 verified listing the disk catalog, saving a file, and loading it again. Hardware photos and screenshots will be added later.
