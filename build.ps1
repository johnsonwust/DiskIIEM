# ============================================================================
#  build.ps1 — DiskIIEM 可复用构建脚本 (Windows 原生)
# ============================================================================
#  用法:
#    .\build.ps1                     # 默认输出到 build\
#    .\build.ps1 -BuildDir build2    # 自定义输出目录
#
#  产出: <BuildDir>\DiskIIEM.uf2 (+ .elf / .bin / .hex / .dis / .elf.map)
#
#  依赖 (脚本只检测, 不负责安装):
#    - ARM 交叉工具链 arm-none-eabi-gcc   (本项目环境来自 STM32CubeCLT)
#    - CMake + GNU Make / mingw32-make
#    - MinGW-w64 (用于编译宿主工具 pioasm / picotool)
#    - Git、Python 3
#
#  脚本内置处理的 4 个 Windows 环境坑:
#    1) Ninja 跑 cmd.exe /C 可能挂起            -> 统一用 "MinGW Makefiles" 生成器
#    2) Git schannel 证书报 SEC_E_NO_CREDENTIALS -> 用 GIT_CONFIG 环境变量强制 openssl 后端
#    3) SDK 自动 FetchContent 拉 picotool 会调 sh.exe 失败 -> 手动克隆 + 编译 picotool
#    4) picotool.exe 启动崩溃 (DLL 冲突)         -> 运行时把 MinGW\bin 提到 PATH 最前
# ============================================================================

[CmdletBinding()]
param(
    [string]$BuildDir = 'build'
)

$ErrorActionPreference = 'Stop'

$ProjectRoot   = $PSScriptRoot
$SdkTag        = '2.2.0'
$PicotoolTag   = '2.2.0'
$SdkDir        = Join-Path $ProjectRoot 'pico-sdk'
$PicotoolSrc   = Join-Path $ProjectRoot '.tools\picotool'
$PicotoolBuild = Join-Path $ProjectRoot 'build-picotool'
$PicotoolExe   = Join-Path $PicotoolBuild 'picotool.exe'
$BuildDir      = Join-Path $ProjectRoot $BuildDir
$Elf           = Join-Path $BuildDir 'DiskIIEM.elf'
$Uf2           = Join-Path $BuildDir 'DiskIIEM.uf2'
$jobs          = if ($env:NUMBER_OF_PROCESSORS) { [int]$env:NUMBER_OF_PROCESSORS } else { 4 }

Write-Host "项目目录: $ProjectRoot" -ForegroundColor Cyan

# ---- 1. 定位工具链 ----
Write-Host '[1/6] 定位工具链' -ForegroundColor Cyan
if (-not $env:PICO_TOOLCHAIN_PATH) {
    $gcc = Get-Command arm-none-eabi-gcc -ErrorAction SilentlyContinue
    if ($gcc) {
        $env:PICO_TOOLCHAIN_PATH = Split-Path (Split-Path $gcc.Source)
    } elseif (Test-Path 'C:\ST\STM32CubeCLT_1.22.0\GNU-tools-for-STM32\bin\arm-none-eabi-gcc.exe') {
        $env:PICO_TOOLCHAIN_PATH = 'C:\ST\STM32CubeCLT_1.22.0\GNU-tools-for-STM32'
    }
}
if (-not $env:PICO_TOOLCHAIN_PATH -or -not (Test-Path (Join-Path $env:PICO_TOOLCHAIN_PATH 'bin\arm-none-eabi-gcc.exe'))) {
    throw '未找到 arm-none-eabi-gcc 工具链, 请设置环境变量 PICO_TOOLCHAIN_PATH 指向工具链根目录'
}

$MingwBin = 'C:\mingw64\bin'
if (-not (Test-Path (Join-Path $MingwBin 'g++.exe'))) {
    $gpp = Get-Command g++ -ErrorAction SilentlyContinue
    if ($gpp) { $MingwBin = Split-Path $gpp.Source } else { throw '未找到 MinGW-w64 (g++), 宿主工具无法编译' }
}
Write-Host "  ARM 工具链 : $env:PICO_TOOLCHAIN_PATH"
Write-Host "  MinGW 宿主 : $MingwBin"

# ---- 2. 强制 git 使用 openssl 后端 (规避 schannel 证书问题) ----
$env:GIT_CONFIG_COUNT   = '1'
$env:GIT_CONFIG_KEY_0   = 'http.sslBackend'
$env:GIT_CONFIG_VALUE_0 = 'openssl'

# ---- 3. Pico SDK ----
Write-Host '[2/6] Pico SDK' -ForegroundColor Cyan
if (-not (Test-Path (Join-Path $SdkDir 'pico_sdk_init.cmake'))) {
    Write-Host "  克隆 pico-sdk $SdkTag (shallow)..."
    git clone --branch $SdkTag --depth 1 https://github.com/raspberrypi/pico-sdk.git $SdkDir
    if ($LASTEXITCODE -ne 0) { throw 'pico-sdk 克隆失败' }
} else {
    Write-Host '  已存在, 跳过克隆'
}

# ---- 4. picotool ----
Write-Host '[3/6] picotool' -ForegroundColor Cyan
if (-not (Test-Path $PicotoolExe)) {
    if (-not (Test-Path (Join-Path $PicotoolSrc 'CMakeLists.txt'))) {
        Write-Host "  克隆 picotool $PicotoolTag (shallow)..."
        git clone --branch $PicotoolTag --depth 1 https://github.com/raspberrypi/picotool.git $PicotoolSrc
        if ($LASTEXITCODE -ne 0) { throw 'picotool 克隆失败' }
    }
    Write-Host '  编译 picotool (MinGW, 无 USB / 无 mbedtls)...'
    $env:PICO_SDK_PATH = $SdkDir
    cmake -G 'MinGW Makefiles' -S $PicotoolSrc -B $PicotoolBuild `
        -DPICOTOOL_NO_LIBUSB=1 -DPICOTOOL_FLAT_INSTALL=1 "-DPICO_SDK_PATH=$SdkDir"
    if ($LASTEXITCODE -ne 0) { throw 'picotool 配置失败' }
    cmake --build $PicotoolBuild --parallel $jobs
    if ($LASTEXITCODE -ne 0) { throw 'picotool 编译失败' }
} else {
    Write-Host '  已存在, 跳过编译'
}

# ---- 5. 配置 + 编译项目 ----
Write-Host '[4/6] 配置项目 (MinGW Makefiles)' -ForegroundColor Cyan
$env:PICO_SDK_PATH = $SdkDir
cmake -G 'MinGW Makefiles' -S $ProjectRoot -B $BuildDir -DCMAKE_BUILD_TYPE=Release -DPICO_NO_PICOTOOL=1
if ($LASTEXITCODE -ne 0) { throw 'CMake 配置失败' }

Write-Host '[5/6] 编译项目' -ForegroundColor Cyan
cmake --build $BuildDir --parallel $jobs
if ($LASTEXITCODE -ne 0) { throw '编译失败' }

# ---- 6. 生成 UF2 (仅此步把 MinGW 提到 PATH 最前, 避免 DLL 冲突) ----
Write-Host '[6/6] 生成 UF2' -ForegroundColor Cyan
$env:PATH = "$MingwBin;$env:PATH"
& $PicotoolExe uf2 convert --quiet $Elf $Uf2 --family rp2350-arm-s --abs-block
if ($LASTEXITCODE -ne 0) { throw 'UF2 生成失败' }

# ---- 汇总 ----
Write-Host ''
Write-Host '完成。产物:' -ForegroundColor Green
Get-ChildItem $BuildDir -File | Where-Object { $_.Name -like 'DiskIIEM.*' -and $_.Extension -in '.uf2', '.elf', '.bin', '.hex' } |
    ForEach-Object { Write-Host ("  {0,-42} {1,8:N1} KB" -f $_.Name, ($_.Length / 1KB)) }
Write-Host ''
& $PicotoolExe info $Uf2
