# Generated Cmake Pico project file

cmake_minimum_required(VERSION 3.13)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Initialise pico_sdk from installed location
# (note this can come from environment, CMake cache etc)

# == DO NOT EDIT THE FOLLOWING LINES for the Raspberry Pi Pico VS Code Extension to work ==
if(WIN32)
    set(USERHOME $ENV{USERPROFILE})
else()
    set(USERHOME $ENV{HOME})
endif()
set(sdkVersion 2.2.0)
set(toolchainVersion 14_2_Rel1)
set(picotoolVersion 2.2.0-a4)
set(picoVscode ${USERHOME}/.pico-sdk/cmake/pico-vscode.cmake)
if (EXISTS ${picoVscode})
    include(${picoVscode})
endif()
# ====================================================================================
# Changed to pico2 for RP2350 (Pico 2) microcontroller
# Using ARM Cortex-M0+ cores instead of RISC-V
set(PICO_BOARD pico2 CACHE STRING "Board type")
set(PICO_PLATFORM rp2350-arm-s CACHE STRING "Platform: rp2040, rp2350-arm-s, rp2350-riscv")

# Pull in Raspberry Pi Pico SDK (must be before project)
include(pico_sdk_import.cmake)

project(DiskIIEM C CXX ASM)

# Initialise the Raspberry Pi Pico SDK
pico_sdk_init()

# Add executable. Default name is the project name, version 0.1

add_executable(DiskIIEM 
    DiskIIEM.cpp
    FloppyEmulator.cpp
    DiskCodec.cpp
    SDCardManager.cpp
    FAT32.cpp
    ST7567.cpp
    Display.cpp
    Buttons.cpp
    Beeper.cpp
    ConfigManager.cpp
    SDFormat.cpp
    UIHandler.cpp
    UIKeys.cpp
    UIRender.cpp
    WozImage.cpp
)

# Name and version stored in the binary: taken from Branding.h
file(STRINGS ${CMAKE_CURRENT_LIST_DIR}/Branding.h BRAND_NAME_LINE REGEX "^#define PRODUCT_NAME ")
file(STRINGS ${CMAKE_CURRENT_LIST_DIR}/Branding.h BRAND_VERSION_LINE REGEX "^#define FW_VERSION ")
string(REGEX REPLACE ".*\"(.*)\".*" "\\1" BRAND_NAME "${BRAND_NAME_LINE}")
string(REGEX REPLACE ".*\"(.*)\".*" "\\1" BRAND_VERSION "${BRAND_VERSION_LINE}")
pico_set_program_name(DiskIIEM "${BRAND_NAME}")
pico_set_program_version(DiskIIEM "${BRAND_VERSION}")

# UART debug/CLI removed — no serial output for now (frees GP0/GP1 and GP4/GP5)
pico_enable_stdio_uart(DiskIIEM 0)
pico_enable_stdio_usb(DiskIIEM 0)

# Generate PIO header
pico_generate_pio_header(DiskIIEM ${CMAKE_CURRENT_LIST_DIR}/floppy_bit_output.pio)

# Add the standard library to the build
target_link_libraries(DiskIIEM
        pico_stdlib
        hardware_spi
        hardware_pio
        hardware_dma
        hardware_pwm
        pico_multicore)

# RAM: the program runs from RAM, so code size is RAM.
#  - Function-local static objects are only created in main() before the
#    second core starts. Without the thread-safe guards the C++ library's
#    exception and name-demangling code (about 25 KB) is not linked.
#  - No text uses floating point formats.
target_compile_options(DiskIIEM PRIVATE $<$<COMPILE_LANGUAGE:CXX>:-fno-threadsafe-statics>)
target_compile_definitions(DiskIIEM PRIVATE
        PICO_PRINTF_SUPPORT_FLOAT=0
        PICO_PRINTF_SUPPORT_EXPONENTIAL=0)

# Add the standard include files to the build
target_include_directories(DiskIIEM PRIVATE
        ${CMAKE_CURRENT_LIST_DIR}
)

#pico_set_binary_type(DiskIIEM no_flash)

pico_set_binary_type(DiskIIEM copy_to_ram)
pico_add_extra_outputs(DiskIIEM)





