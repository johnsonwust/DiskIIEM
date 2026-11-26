# DiskIIEM · Pico 2 Firmware

[简体中文](README.zh-CN.md)

DiskIIEM is a 5.25-inch floppy drive emulator for Apple II systems with a Disk II controller card. It runs on a Raspberry Pi Pico 2 (RP2350) and presents disk images on an SD card to the Apple II as floppy disks. Its LCD and four buttons let you browse, mount, and swap images directly on the device.

This repository contains the RP2350 firmware, pin assignments, and build scripts. The firmware version declared in the source is **v1.2.1**. [Version definition](Branding.h)

## Specifications

| Item | Specification |
| --- | --- |
| Host interface | 5.25-inch Apple II Disk II drive interface. Four stepper phase inputs, drive selection, write enable, write data input, read data output, and write protect output |
| Controller | Raspberry Pi Pico 2, RP2350. The CMake build targets `pico2` and `rp2350-arm-s` |
| Clock and execution | Firmware sets the system clock to 200 MHz and the internal regulator to 1.20 V at startup. The program is copied to RAM for execution |
| Disk images | `.dsk` (DOS sector order), `.nic`, and `.woz` (WOZ 1 and WOZ 2 for 5.25-inch disks) |
| SD card | FAT32. SD on SPI0 with a 400 kHz initialization clock and a 10 MHz operating clock requested by the firmware |
| Display | 128 × 64 monochrome ST7567 LCD on SPI1, initialized at 500 kHz. Active-low backlight, with a 15-second timeout when a card is present |
| Controls | Four active-low buttons for Up, Down, Return, and Enter. Hold Up or Down to scroll |
| Audio | Seek and button sounds through a PWM buzzer output. The firmware configures 2 kHz at 10% duty cycle |
| Diagnostics | SD card and image status on the LCD. The Config menu can export a write log and a raw track dump to the SD card |

Hardware specifications reflect the wiring described in [PinConfig.h](PinConfig.h). Build targets and clock settings are defined in [CMakeLists.txt](CMakeLists.txt) and [DiskIIEM.cpp](DiskIIEM.cpp).

## Disk image support

| Format | On-disk layout | Emulation and write-back |
| --- | --- | --- |
| DSK | 143,360 bytes (140 KiB). 35 tracks × 16 sectors × 256 bytes, in DOS order | The firmware builds GCR tracks from sector data and decodes written sectors back into the image. Standard 16-sector layout |
| NIC | 286,720 bytes (280 KiB). 35 tracks × 16 slots of 512 bytes, with 416 GCR track bytes per slot | The firmware removes slot padding for playback and writes decoded sectors back in the slot layout. Standard 16-sector layout |
| WOZ 1 / WOZ 2 | Track bitstreams with a quarter-track map. Track lengths and bit timing can vary | Supports bitstream reads and writes on eligible tracks. WOZ 2 FLUX tracks can be played back and are read-only. Large WOZ 2 files use a track cache backed by the SD card. WOZ 1 must fit in the image buffer |

The file browser shows these three extensions. The firmware targets the 5.25-inch Disk II drive interface. DSK and NIC write back data using standard 16-sector layouts. Use WOZ when an image needs to preserve a nonstandard track layout or bitstream. Compatibility with individual protected titles depends on their disk layout and timing.

DSK and NIC use 53,248 bits per track at a nominal 4 µs per bit in this firmware. That works out to about 213 ms per revolution (282 rpm), so software that measures spindle speed may report a lower value than 300 rpm. [Track constants](FloppyEmulator.h) · [Design and compatibility notes](WOZ_DESIGN.md)

## How it works

### Timing and core responsibilities

- **Core 0** services the Disk II interface. It tracks head position, prepares track data, captures writes, and schedules pending SD card writes.
- **Core 1** handles the display, buttons, file browser, and SD operations started from the menu, such as mounting images and formatting a card.
- **PIO and DMA** keep the read signal moving independently of individual CPU instructions. The PIO state machine clocks out track bits. Two chained DMA channels feed it from alternating 64-byte buffers, while an interrupt refills the free buffer.

### Reading and seeking

The controller's four phase signals move the virtual head when the drive is selected. For DSK, the firmware builds a Group Code Recording (GCR) track using 4-and-4 encoding for address fields and 6-and-2 encoding for data fields. For NIC it unpacks the existing track data. For WOZ it selects a track through the image's quarter-track map. At the default 4 µs bit cell, a `1` appears as a 1 µs high pulse on `RD_DATA`. A `0` leaves the line low for the entire cell. WOZ playback uses the image's bit timing and preserves angular position when switching between tracks of different lengths. The Config menu also lets you enable weak-bit simulation.

### Writing and saving

With `DRIVE_SEL#` and `WRITE_ENABLE#` active, the firmware pauses read playback and uses a free-running 40 MHz timer to measure `WR_DATA` transitions in 25 ns ticks. It turns the intervals into bits and merges them into the current track. A 300 µs window keeps nearby write bursts in the same session, which matters when DOS 3.3 writes a sector's address and data fields separately.

The firmware commits the updated track to the image in memory. DSK and NIC writes are decoded and stored as sectors. WOZ writes update existing bitstream tracks in place, keeping the original track layout. The WOZ loader validates track boundaries and any nonzero file CRC. The writer clears that CRC field before the first in-place track update.

Pending changes are normally saved to the card after drive deselection. Failed writes stay pending for retry. Wait for `UNSAVED` to clear before switching off the Apple II or removing the card.

[Emulation and write-back code](FloppyEmulator.cpp) · [PIO program](floppy_bit_output.pio) · [WOZ parser](WozImage.cpp)

## Hardware pin assignments

The table lists **Pico GPIO numbers**. Use the board schematic for connector wiring and power. The current board uses series diodes and 2.2 kΩ pull-ups to 3.3 V on the seven Disk II signal inputs. A non-inverting 74HC125 buffers `RD_DATA` and `WR_PROTECT`. The drive-select signal controls their output enables so the unselected drive releases the shared lines. The earlier demo board uses inverted outputs. Set `READ_OUTPUT_INVERTED` and `WPROT_OUTPUT_INVERTED` to `1` for that board.

| Signal or function | Pico GPIO | Direction and behavior |
| --- | --- | --- |
| `PH0`, `PH1`, `PH2`, `PH3` | GP2, GP3, GP4, GP5 | Inputs, active high |
| `RD_DATA` | GP10 | Output, idle low, high pulse for a `1` |
| `WR_DATA` | GP11 | Input, transitions encode writes |
| `WRITE_ENABLE#` | GP12 | Input, active low |
| `DRIVE_SEL#` | GP13 | Input, active low |
| `WR_PROTECT` | GP14 | Output, high means write-protected |
| SD card detect | GP15 | Input, active low |
| SD SPI0 | GP16 MISO, GP17 CS, GP18 SCK, GP19 MOSI | Card data and control |
| LCD SPI1 | GP26 SCK, GP27 MOSI, GP9 CS | Display data and control |
| LCD A0, reset, backlight | GP20, GP22, GP21 | Outputs. Backlight turns on at low level |
| Down, Up, Return, Enter | GP6, GP7, GP8, GP28 | Button inputs, active low |
| Buzzer | GP0 | PWM output, low when silent |

[Pin map and board notes](PinConfig.h)

## Setup and operation

1. Use a FAT32-formatted SD card and copy a standard 140 KiB DOS-order `.dsk` image to it. You can organize more images in folders.
2. Power off the Apple II. Connect the assembled DiskIIEM device to the Disk II controller card, insert the SD card, and turn on the Apple II.
3. Open **Files**, select an image with Up or Down, then press Enter to mount it. Return moves up a folder or back to the home screen.
4. Boot from the mounted image or access it from the Apple II as you would a floppy disk. After writing, wait for the pending save indicator to clear.

| Menu | What you can do |
| --- | --- |
| **Files** | Browse subdirectories, mount images, and switch disks. The browser sorts folders before files and shows a window of six entries without loading the entire directory into RAM |
| **Status** | Check card type, SPI speed, volume size, partition start, and cluster size |
| **Info** | Check image type, current track, write protection, and save status. WOZ images can also show quarter tracks and bit timing |
| **Config** | Change sound, autoload, and weak-bit settings. Toggle write protection for the mounted image, format the SD card, or export diagnostics |

Settings and the last mounted image are stored in `config.cfg` at the root of each card. Write protection follows the image file's FAT32 read-only attribute. A WOZ image with internal write protection remains protected even if you clear the file attribute. **Format SD** erases the card's files and creates a single FAT32 partition. Back up the card before using it.

If you see `NO SD CARD`, reseat the card and allow the device to retry. If you see `exFAT detected!`, copy your files off the card and format it as FAT32 before using it. For a reproducible read or write issue, **Config → Save debug log** creates `WLOGnn.TXT` (signal levels and recent write events) and `WTRKnn.BIN` (current track data). [Menu reference](UI_MENU.txt)

## Build and flash

The project uses CMake with Raspberry Pi Pico SDK **2.2.0** and the `arm-none-eabi-gcc` toolchain. On Windows, install CMake, Git, and MinGW-w64 with its make tool, then run this from the repository root in PowerShell:

```powershell
.\build.ps1
```

The script fetches the Pico SDK and picotool 2.2.0 if they are missing, then produces `build/DiskIIEM.uf2`. If it cannot find the ARM compiler, set `PICO_TOOLCHAIN_PATH` to the toolchain root. To flash, hold **BOOTSEL** while connecting the Pico 2 to your computer by USB, then copy the UF2 to the drive that appears. [Build script](build.ps1) · [CMake configuration](CMakeLists.txt)

## Project files

| File or group | Purpose |
| --- | --- |
| [DiskIIEM.cpp](DiskIIEM.cpp) | Startup, core setup, and runtime loop |
| [FloppyEmulator.cpp](FloppyEmulator.cpp), [DiskCodec.cpp](DiskCodec.cpp), [floppy_bit_output.pio](floppy_bit_output.pio) | Track streaming, GCR encoding and decoding, write capture, and persistence |
| [WozImage.cpp](WozImage.cpp) | WOZ headers, track maps, FLUX metadata, and CRC validation |
| [SDCardManager.cpp](SDCardManager.cpp), [FAT32.cpp](FAT32.cpp), [SDFormat.cpp](SDFormat.cpp) | SD access, FAT32 file operations, and card formatting |
| [UIHandler.cpp](UIHandler.cpp), [UIKeys.cpp](UIKeys.cpp), [UIRender.cpp](UIRender.cpp) | Device menus, button navigation, and LCD rendering |
| [PinConfig.h](PinConfig.h), [Branding.h](Branding.h) | Pin assignments, board options, and firmware version |
| [DEVELOPMENT.md](DEVELOPMENT.md), [WOZ_DESIGN.md](WOZ_DESIGN.md) | Development background, implementation history, and verification records |

## Verification status

- **Apple II hardware** — Development records confirm DOS 3.3 `CATALOG`, `SAVE`, and `LOAD` on a prototype.
- **Host simulation** — Historical checks cover sector read and write, full-track formatting, WOZ parsing and playback, FAT32 operations, and menu behavior. Physical PIO waveforms and additional SD card and controller combinations require hardware verification.

Hardware photos and interface screenshots will be added later. [Verification records](WOZ_DESIGN.md)
