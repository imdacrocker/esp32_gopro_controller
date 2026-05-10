# dev.ps1 - daily dev wrapper for the main app.
#
# Usage:
#   .\dev.ps1                      # build + OTA-flash + monitor (default)
#   .\dev.ps1 build                # build only
#   .\dev.ps1 flash                # build + OTA-flash (no monitor)
#   .\dev.ps1 flash-usb            # build + USB-flash main app only (ota_0)
#   .\dev.ps1 monitor              # idf.py monitor
#   .\dev.ps1 -ip 10.71.79.1 ...   # override target IP (default = SoftAP IP)
#   .\dev.ps1 -port COM7 ...       # override serial port
#
# The OTA-flash path POSTs app.bin (and storage.bin if it exists) to the
# device's /api/ota/upload-* endpoints, then /api/ota/commit. Upload is a
# no-op on the device side when the SHA matches NVS (SHA-skip, ota_design.md
# section 6) - useful when only one of the two binaries actually changed.
#
# Requires the IDF environment to be sourced first:
#   & 'C:\esp\v6.0.1\esp-idf\export.ps1'

param(
    [Parameter(Position=0)] [string]$cmd = "all",
    [string]$ip   = "10.71.79.1",
    [string]$port = "COM3"
)

$ErrorActionPreference = "Stop"

$proj = $PSScriptRoot
$bin  = Join-Path $proj "build\esp32_gopro_canbus_controller_v2.bin"
$ui   = Join-Path $proj "build\storage.bin"

function Build   { idf.py -C $proj build }
function Monitor { idf.py -C $proj -p $port monitor }

function Sha256OfFile($path) {
    return (Get-FileHash -Algorithm SHA256 $path).Hash.ToLower()
}

function Curl-Post($url, $headers, $bodyPath) {
    # curl.exe ships with Windows 10/11. Using it because Invoke-RestMethod
    # has trouble with raw binary bodies plus custom headers in PS 5.1.
    $args = @("--fail", "--silent", "--show-error", "-X", "POST")
    foreach ($k in $headers.Keys) { $args += @("-H", "$($k): $($headers[$k])") }
    if ($bodyPath) { $args += @("--data-binary", "@$bodyPath") }
    $args += $url
    & curl.exe @args
    if ($LASTEXITCODE -ne 0) { throw "POST $url failed (exit $LASTEXITCODE)" }
}

function FlashOta {
    if (-not (Test-Path $bin)) { throw "$bin missing - run build first" }

    $appSha  = Sha256OfFile $bin
    $appSize = (Get-Item $bin).Length
    Write-Host "Uploading app.bin ($appSize bytes, sha=$appSha) -> http://$ip ..."
    Curl-Post "http://$ip/api/ota/upload-app" `
              @{ "X-Sha256" = $appSha; "X-Size" = $appSize; "Content-Type" = "application/octet-stream" } `
              $bin

    if (Test-Path $ui) {
        $uiSha  = Sha256OfFile $ui
        $uiSize = (Get-Item $ui).Length
        Write-Host "Uploading storage.bin ($uiSize bytes, sha=$uiSha) ..."
        Curl-Post "http://$ip/api/ota/upload-ui" `
                  @{ "X-Sha256" = $uiSha; "X-Size" = $uiSize; "Content-Type" = "application/octet-stream" } `
                  $ui
    }

    Write-Host "Committing & rebooting ..."
    Curl-Post "http://$ip/api/ota/commit" @{}
    Write-Host "Done. Device should be back on $ip in ~5 s."
}

function FlashUsb {
    # Main app only - for when OTA can't reach the device. Recovery and
    # storage are NOT touched. Use tools\flash_factory.ps1 for a full
    # provisioning of a fresh board (bootloader + recovery + main + storage).
    Build
    Write-Host "USB-flashing main app to ota_0 (0xE0000) on $port ..."
    python -m esptool --chip esp32s3 -p $port -b 921600 write_flash 0xE0000 $bin
}

switch ($cmd) {
    "build"     { Build }
    "flash"     { Build; FlashOta }
    "flash-usb" { FlashUsb }
    "monitor"   { Monitor }
    "all"       { Build; FlashOta; Monitor }
    default     { Write-Host "usage: .\dev.ps1 [build|flash|flash-usb|monitor|all] [-ip IP] [-port COMn]" }
}
