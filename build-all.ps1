$ErrorActionPreference = "Stop"

# ============================================================
# DVPlogger Windows build script
#
#   HW1 -> binaries/mini
#   HW3 -> binaries/Wide
#
# Build order:
#   1. dvplogger_ext
#   2. Generate binaries.c
#   3. Copy SUB CPU binaries
#   4. Install binaries.c into dvplogger/main
#   5. Build dvplogger
#   6. Copy MAIN CPU binaries
# ============================================================

$RootDir  = Split-Path -Parent $MyInvocation.MyCommand.Path
$MainDir  = Join-Path $RootDir "dvplogger"
$ExtDir   = Join-Path $RootDir "dvplogger_ext"
$OutputDir = Join-Path $RootDir "binaries"


function Die {
    param(
        [string]$Message
    )

    Write-Error "ERROR: $Message"
    exit 1
}


function Invoke-Checked {
    param(
        [string]$Command,
        [string[]]$Arguments
    )

    & $Command @Arguments

    if ($LASTEXITCODE -ne 0) {
        Die "$Command failed with exit code $LASTEXITCODE"
    }
}


function Copy-FileSafe {
    param(
        [string]$Source,
        [string]$Destination
    )

    if (-not (Test-Path -LiteralPath $Source -PathType Leaf)) {
        Die "File not found: $Source"
    }

    $DestDir = Split-Path -Parent $Destination

    if (-not (Test-Path -LiteralPath $DestDir)) {
        New-Item -ItemType Directory -Force -Path $DestDir | Out-Null
    }

    $Tmp = "$Destination.tmp"

    Copy-Item -LiteralPath $Source -Destination $Tmp -Force
    Move-Item -LiteralPath $Tmp -Destination $Destination -Force

    Write-Host "  $Source"
    Write-Host "    -> $Destination"
}


function New-Crc32Manifest {
    param(
        [string]$DestDir
    )

    $ManifestScript = Join-Path $RootDir "make_crc32_manifest.py"
    $Manifest = Join-Path $DestDir "CRC32SUMS.txt"

    if (-not (Test-Path -LiteralPath $ManifestScript -PathType Leaf)) {
        Die "CRC32 manifest helper not found: $ManifestScript"
    }

    $Python = Get-Command python -ErrorAction SilentlyContinue
    if (-not $Python) {
        $Python = Get-Command python3 -ErrorAction SilentlyContinue
    }
    if (-not $Python) {
        Die "Python was not found in PATH"
    }

    Write-Host ""
    Write-Host "--- Generating CRC32 manifest ---"

    Invoke-Checked $Python.Source @(
        $ManifestScript,
        $DestDir,
        $Manifest
    )
}


function Copy-ExtBinaries {
    param(
        [string]$BuildDir,
        [string]$DestDir
    )

    Copy-FileSafe `
        (Join-Path $BuildDir "jk1dvplog_ext.bin") `
        (Join-Path $DestDir "app0.bin")

    Copy-FileSafe `
        (Join-Path $BuildDir "bootloader\bootloader.bin") `
        (Join-Path $DestDir "bootload.bin")

    Copy-FileSafe `
        (Join-Path $BuildDir "partition_table\partition-table.bin") `
        (Join-Path $DestDir "partitio.bin")

    Copy-FileSafe `
        (Join-Path $BuildDir "spiffs.bin") `
        (Join-Path $DestDir "spiffs.bin")
}


function Build-Set {
    param(
        [int]$HwVer,
        [string]$ModelDir
    )

    $ExtBuild  = Join-Path $RootDir "build-ext-hw$HwVer"
    $MainBuild = Join-Path $RootDir "build-main-hw$HwVer"

    $DestDir   = Join-Path $OutputDir $ModelDir
    $SubCpuDir = Join-Path $DestDir "subcpu"

    Write-Host ""
    Write-Host "============================================================"
    Write-Host " Building HW$HwVer / $ModelDir"
    Write-Host "============================================================"

    # --------------------------------------------------------
    # 1. Build SUB CPU firmware
    # --------------------------------------------------------

    Write-Host ""
    Write-Host "--- Building dvplogger_ext HW$HwVer ---"

    Invoke-Checked "idf.py" @(
        "-C", $ExtDir,
        "-B", $ExtBuild,
        "-DJK1DVPLOG_HWVER=$HwVer",
        "build"
    )

    # --------------------------------------------------------
    # 2. Generate binaries.c
    # --------------------------------------------------------

    Write-Host ""
    Write-Host "--- Generating binaries.c for HW$HwVer ---"

    $BinariesC = Join-Path $ExtBuild "binaries.c"

    if (Test-Path -LiteralPath $BinariesC) {
        Remove-Item -LiteralPath $BinariesC -Force
    }

    $Bin2ArrayRunner = Join-Path $ExtDir "main\bin2array_runner.cmake"

    Invoke-Checked "cmake" @(
        "-DINPUT_DIR=$ExtBuild",
        "-DOUTPUT_FILE=$BinariesC",
        "-P", $Bin2ArrayRunner
    )

    if (-not (Test-Path -LiteralPath $BinariesC -PathType Leaf)) {
        Die "binaries.c was not generated: $BinariesC"
    }

    $FoundBinary = Select-String `
        -LiteralPath $BinariesC `
        -Pattern "jk1dvplog_ext_bin\[\]" `
        -Quiet

    if (-not $FoundBinary) {
        Die "jk1dvplog_ext.bin was not included in binaries.c"
    }

    # --------------------------------------------------------
    # 3. Copy SUB CPU binaries
    # --------------------------------------------------------

    Write-Host ""
    Write-Host "--- Copying sub-CPU binaries ---"

    Copy-ExtBinaries $ExtBuild $SubCpuDir

    # --------------------------------------------------------
    # 4. Install generated binaries.c into MAIN firmware
    # --------------------------------------------------------

    Write-Host ""
    Write-Host "--- Installing binaries.c into main firmware ---"

    $MainBinariesC = Join-Path $MainDir "main\binaries.c"

    Copy-FileSafe $BinariesC $MainBinariesC

    # --------------------------------------------------------
    # 5. Build MAIN CPU firmware
    # --------------------------------------------------------

    Write-Host ""
    Write-Host "--- Building dvplogger HW$HwVer ---"

    Invoke-Checked "idf.py" @(
        "-C", $MainDir,
        "-B", $MainBuild,
        "-DJK1DVPLOG_HWVER=$HwVer",
        "build"
    )

    # --------------------------------------------------------
    # 6. Copy MAIN CPU binaries
    # --------------------------------------------------------

    Write-Host ""
    Write-Host "--- Copying main firmware ---"

    Copy-FileSafe `
        (Join-Path $MainBuild "dvplogger-hw$HwVer.bin") `
        (Join-Path $DestDir "dvplogger.bin")

    Copy-FileSafe `
        (Join-Path $MainBuild "bootloader\bootloader.bin") `
        (Join-Path $DestDir "bootloader.bin")

    Copy-FileSafe `
        (Join-Path $MainBuild "partition_table\partition-table.bin") `
        (Join-Path $DestDir "partition-table.bin")

    New-Crc32Manifest $DestDir

    Write-Host ""
    Write-Host "HW$HwVer / $ModelDir completed."
}


# ============================================================
# Sanity checks
# ============================================================

if (-not (Test-Path -LiteralPath $MainDir -PathType Container)) {
    Die "Directory not found: $MainDir"
}

if (-not (Test-Path -LiteralPath $ExtDir -PathType Container)) {
    Die "Directory not found: $ExtDir"
}

# Check Arduino component, because Windows symlink/junction handling
# can otherwise produce confusing Arduino.h errors.

$MainArduinoH = Join-Path $MainDir "components\arduino\cores\esp32\Arduino.h"
$ExtArduinoH  = Join-Path $ExtDir  "components\arduino\cores\esp32\Arduino.h"

if (-not (Test-Path -LiteralPath $MainArduinoH -PathType Leaf)) {
    Die "Arduino.h not found in main component: $MainArduinoH"
}

if (-not (Test-Path -LiteralPath $ExtArduinoH -PathType Leaf)) {
    Die "Arduino.h not found through dvplogger_ext Arduino component: $ExtArduinoH"
}


# ============================================================
# Build both hardware versions
# ============================================================

Build-Set 1 "mini"
Build-Set 3 "Wide"


# ============================================================
# Summary
# ============================================================

Write-Host ""
Write-Host "============================================================"
Write-Host " All builds completed successfully"
Write-Host "============================================================"

Write-Host ""
Write-Host "HW1 / mini:"
Write-Host "  binaries/mini/dvplogger.bin"
Write-Host "  binaries/mini/bootloader.bin"
Write-Host "  binaries/mini/partition-table.bin"
Write-Host "  binaries/mini/subcpu/app0.bin"
Write-Host "  binaries/mini/subcpu/bootload.bin"
Write-Host "  binaries/mini/subcpu/partitio.bin"
Write-Host "  binaries/mini/subcpu/spiffs.bin"
Write-Host "  binaries/mini/CRC32SUMS.txt"

Write-Host ""
Write-Host "HW3 / Wide:"
Write-Host "  binaries/Wide/dvplogger.bin"
Write-Host "  binaries/Wide/bootloader.bin"
Write-Host "  binaries/Wide/partition-table.bin"
Write-Host "  binaries/Wide/subcpu/app0.bin"
Write-Host "  binaries/Wide/subcpu/bootload.bin"
Write-Host "  binaries/Wide/subcpu/partitio.bin"
Write-Host "  binaries/Wide/subcpu/spiffs.bin"
Write-Host "  binaries/Wide/CRC32SUMS.txt"
