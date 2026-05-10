# tools\flash_factory.ps1 — one-shot USB provisioning of a fresh board.
#
# Builds both projects and writes the full flash layout. Use for a brand-new
# device, after a `--erase-flash`, or when OTA has gotten the device into a
# state recovery can't recover from. For daily dev use .\dev.ps1 instead.
#
# Sibling project layout assumed:
#   <parent>\esp32_gopro_canbus_controller_v2\   (this script lives in tools\)
#   <parent>\esp32_gopro_canbus_recovery\
#
# Requires the IDF environment to be sourced first:
#   & 'C:\esp\v6.0.1\esp-idf\export.ps1'

param(
    [string]$port = "COM3"
)

$ErrorActionPreference = "Stop"

$mainProj = Resolve-Path (Join-Path $PSScriptRoot "..")
$recProj  = Resolve-Path (Join-Path $PSScriptRoot "..\..\esp32_gopro_canbus_recovery")
$mainBld  = Join-Path $mainProj "build"
$recBld   = Join-Path $recProj  "build"

Write-Host "Building main app: $mainProj"
idf.py -C $mainProj build
Write-Host "Building recovery:  $recProj"
idf.py -C $recProj  build

# Partition layout (see esp32_gopro_canbus_controller_v2\partitions.csv):
#   0x000000  bootloader.bin
#   0x008000  partition-table.bin
#   0x00F000  ota_data_initial.bin   — points the bootloader at ota_0
#   0x020000  recovery (factory)     — 768 KB
#   0x0E0000  main app (ota_0)       — 1.69 MB
#   0x290000  storage (LittleFS)     — 3 MB
#   0x590000  ota_1                  — left blank, populated on first OTA
Write-Host "Flashing $port ..."
python -m esptool --chip esp32s3 -p $port -b 921600 write_flash `
    0x000000 (Join-Path $mainBld "bootloader\bootloader.bin") `
    0x008000 (Join-Path $mainBld "partition_table\partition-table.bin") `
    0x00F000 (Join-Path $mainBld "ota_data_initial.bin") `
    0x020000 (Join-Path $recBld  "esp32_gopro_canbus_recovery.bin") `
    0x0E0000 (Join-Path $mainBld "esp32_gopro_canbus_controller_v2.bin") `
    0x290000 (Join-Path $mainBld "storage.bin")

Write-Host ""
Write-Host "Factory provisioning complete. Power-cycle the board."
Write-Host "Device should come up on SoftAP HERO-RC-XXXXXX, IP 10.71.79.1."
