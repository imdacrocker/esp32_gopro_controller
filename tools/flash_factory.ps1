# tools\flash_factory.ps1 — one-shot USB provisioning of a fresh board.
#
# Builds the chosen variant app + recovery, merges them into a single
# factory.bin, and writes it at 0x0. The merged image is the same artifact
# CI publishes as a release asset and that ESP Launchpad consumes — keeping
# local and remote provisioning byte-for-byte consistent.
#
# Use for a brand-new device, after a `--erase-flash`, or when OTA has
# gotten the device into a state recovery can't recover from. For daily
# dev use .\dev.ps1 instead.
#
# Variant selection:
#   .\tools\flash_factory.ps1                  # wireless (default)
#   .\tools\flash_factory.ps1 -Product wired   # wired (USB-host) variant
#
# Both variants share partitions.csv, the recovery app, and the SoftAP web
# UI; only the ota_0 app + its storage.bin differ. Recovery is a single tree
# stamped with CONFIG_PRODUCT_VARIANT at build time so its OTA route matches
# the paired main app — this script stamps it (and the app) before building,
# mirroring the CI release workflow.
#
# Monorepo layout (this script lives in tools\ at the repo root):
#   <repo>\apps\wireless\
#   <repo>\apps\wired\
#   <repo>\apps\recovery\
#
# Requires the IDF environment to be sourced first:
#   & 'C:\esp\v6.0.1\esp-idf\export.ps1'
#
# Serial port auto-detected by USB VID/PID — pass -port COMn to override.

param(
    [ValidateSet("wireless","wired")] [string]$Product = "wireless",
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

$repo    = Resolve-Path (Join-Path $PSScriptRoot "..")
$appProj = Resolve-Path (Join-Path $repo "apps\$Product")
$recProj = Resolve-Path (Join-Path $repo "apps\recovery")
$appBld  = Join-Path $appProj "build"
$recBld  = Join-Path $recProj "build"
$outDir  = Join-Path $repo "build"
$factory = Join-Path $outDir "factory.bin"
$appBin  = Join-Path $appBld "esp32_gopro_canbus_$Product.bin"

function Stamp-ProductVariant {
    # Stamp CONFIG_PRODUCT_VARIANT into the variant app + recovery
    # sdkconfig.defaults so /api/version reports the right slug and recovery's
    # OTA route targets the paired variant. Set it explicitly even when it
    # matches the Kconfig default, so a typo there can't bake the wrong
    # variant (mirrors release-beta.yml's stamping step). Idempotent.
    $targets = @(
        (Join-Path $repo "apps\$Product\sdkconfig.defaults"),
        (Join-Path $repo "apps\recovery\sdkconfig.defaults")
    )
    $desired = "CONFIG_PRODUCT_VARIANT=`"$Product`""
    foreach ($f in $targets) {
        if (-not (Test-Path $f)) { continue }
        $content = Get-Content $f -Raw
        if ($content -match '(?m)^CONFIG_PRODUCT_VARIANT=.*$') {
            $new = [regex]::Replace($content, '(?m)^CONFIG_PRODUCT_VARIANT=.*$', $desired)
            if ($new -ne $content) { Set-Content -Path $f -Value $new -NoNewline:$false }
        } else {
            Add-Content -Path $f -Value $desired
        }
    }
}

Stamp-ProductVariant

Write-Host "Building $Product app: $appProj"
idf.py -C $appProj build
Write-Host "Building recovery:     $recProj"
idf.py -C $recProj build

if (-not (Test-Path $outDir)) { New-Item -ItemType Directory -Path $outDir | Out-Null }

# IDF emits ota_data_initial.bin as all-0xFF, which the bootloader
# interprets as "boot factory" when a factory partition exists. Rewrite
# it to a valid seq=1 entry so first-boot lands in ota_0 (the variant app).
Write-Host "Stamping ota_data_initial.bin to select ota_0"
python "$PSScriptRoot\release\make_factory_otadata.py" `
    (Join-Path $appBld "ota_data_initial.bin")

# Partition layout (see partitions.csv at repo root):
#   0x000000  bootloader.bin
#   0x008000  partition-table.bin
#   0x00F000  ota_data_initial.bin   — stamped above to select ota_0
#   0x020000  recovery (factory)     — 768 KB
#   0x0E0000  variant app (ota_0)    — 1.69 MB
#   0x290000  storage (LittleFS)     — 3 MB
#   0x590000  ota_1                  — left blank, populated on first OTA
Write-Host "Merging factory image -> $factory"
python -m esptool --chip esp32s3 merge-bin `
    -o $factory `
    --flash_mode dio --flash_size 8MB --flash_freq 80m `
    0x000000 (Join-Path $appBld "bootloader\bootloader.bin") `
    0x008000 (Join-Path $appBld "partition_table\partition-table.bin") `
    0x00F000 (Join-Path $appBld "ota_data_initial.bin") `
    0x020000 (Join-Path $recBld "esp32_gopro_canbus_recovery.bin") `
    0x0E0000 $appBin `
    0x290000 (Join-Path $appBld "storage.bin")

$p = Resolve-Port
Write-Host "Flashing $p ..."
python -m esptool --chip esp32s3 -p $p -b 921600 write-flash 0x0 $factory

Write-Host ""
Write-Host "Factory provisioning complete ($Product). Power-cycle the board."
Write-Host "Device should come up on SoftAP HERO-RC-XXXXXX, IP 10.71.79.1."
