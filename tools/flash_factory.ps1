# tools\flash_factory.ps1 — one-shot USB provisioning of a fresh board.
#
# Builds both projects, merges them into a single factory.bin, and writes
# it at 0x0. The merged image is the same artifact CI publishes as a
# release asset and that ESP Launchpad consumes — keeping local and
# remote provisioning byte-for-byte consistent.
#
# Use for a brand-new device, after a `--erase-flash`, or when OTA has
# gotten the device into a state recovery can't recover from. For daily
# dev use .\dev.ps1 instead.
#
# Monorepo layout (this script lives in tools\ at the repo root):
#   <repo>\apps\wireless\
#   <repo>\apps\recovery\
#
# Requires the IDF environment to be sourced first:
#   & 'C:\esp\v6.0.1\esp-idf\export.ps1'
#
# Serial port auto-detected by USB VID/PID — pass -port COMn to override.

param(
    [string]$port
)

$ErrorActionPreference = "Stop"

# Auto-detect the ESP32 serial port by USB VID/PID — same matcher used by
# dev.ps1 (and by the ESP-IDF VS Code extension under the hood). Covers the
# ESP32-S3 native USB-JTAG/Serial device plus the two USB-UART bridges
# commonly fitted to ESP32 devkits.
function Get-EspPort {
    $candidates = Get-PnpDevice -Class Ports -Status OK -ErrorAction SilentlyContinue |
        Where-Object {
            $_.InstanceId -match 'VID_303A&PID_1001' -or  # ESP32-S3 native USB
            $_.InstanceId -match 'VID_10C4&PID_EA60' -or  # CP210x
            $_.InstanceId -match 'VID_1A86'                # CH340
        }
    $ports = @(foreach ($d in $candidates) {
        if ($d.FriendlyName -match '\((COM\d+)\)') { $matches[1] }
    })
    if (-not $ports) { throw "No ESP32 serial port detected. Plug in the board or pass -port COMn." }
    if ($ports.Count -gt 1) {
        Write-Host "Multiple ESP boards found ($($ports -join ', ')); using $($ports[0]). Pass -port to override."
    }
    return $ports[0]
}

function Resolve-Port {
    if (-not $script:port) { $script:port = Get-EspPort }
    return $script:port
}

$wirelessProj = Resolve-Path (Join-Path $PSScriptRoot "..\apps\wireless")
$recProj      = Resolve-Path (Join-Path $PSScriptRoot "..\apps\recovery")
$wirelessBld  = Join-Path $wirelessProj "build"
$recBld       = Join-Path $recProj      "build"
$outDir       = Join-Path $PSScriptRoot "..\build"
$factory      = Join-Path $outDir "factory.bin"

Write-Host "Building wireless app: $wirelessProj"
idf.py -C $wirelessProj build
Write-Host "Building recovery:     $recProj"
idf.py -C $recProj      build

if (-not (Test-Path $outDir)) { New-Item -ItemType Directory -Path $outDir | Out-Null }

# IDF emits ota_data_initial.bin as all-0xFF, which the bootloader
# interprets as "boot factory" when a factory partition exists. Rewrite
# it to a valid seq=1 entry so first-boot lands in ota_0 (wireless app).
Write-Host "Stamping ota_data_initial.bin to select ota_0"
python "$PSScriptRoot\release\make_factory_otadata.py" `
    (Join-Path $wirelessBld "ota_data_initial.bin")

# Partition layout (see partitions.csv at repo root):
#   0x000000  bootloader.bin
#   0x008000  partition-table.bin
#   0x00F000  ota_data_initial.bin   — stamped above to select ota_0
#   0x020000  recovery (factory)     — 768 KB
#   0x0E0000  wireless app (ota_0)   — 1.69 MB
#   0x290000  storage (LittleFS)     — 3 MB
#   0x590000  ota_1                  — left blank, populated on first OTA
Write-Host "Merging factory image -> $factory"
python -m esptool --chip esp32s3 merge-bin `
    -o $factory `
    --flash_mode dio --flash_size 8MB --flash_freq 80m `
    0x000000 (Join-Path $wirelessBld "bootloader\bootloader.bin") `
    0x008000 (Join-Path $wirelessBld "partition_table\partition-table.bin") `
    0x00F000 (Join-Path $wirelessBld "ota_data_initial.bin") `
    0x020000 (Join-Path $recBld      "esp32_gopro_canbus_recovery.bin") `
    0x0E0000 (Join-Path $wirelessBld "esp32_gopro_canbus_wireless.bin") `
    0x290000 (Join-Path $wirelessBld "storage.bin")

$p = Resolve-Port
Write-Host "Flashing $p ..."
python -m esptool --chip esp32s3 -p $p -b 921600 write-flash 0x0 $factory

Write-Host ""
Write-Host "Factory provisioning complete. Power-cycle the board."
Write-Host "Device should come up on SoftAP HERO-RC-XXXXXX, IP 10.71.79.1."
