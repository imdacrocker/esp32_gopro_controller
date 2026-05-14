# Development

How to build, flash, and iterate on the firmware.

## Prerequisites

- **ESP-IDF v6.0.1** (the project pins this version explicitly in the workflows)
- **Target:** `esp32s3`
- **PowerShell** (`dev.ps1` and `flash_factory.ps1` are PS scripts)
- **`curl.exe`** (ships with Windows 10/11) for the OTA upload helpers

Source the IDF environment once per shell:

```powershell
& 'C:\esp\v6.0.1\esp-idf\export.ps1'
```

---

## Daily dev — `dev.ps1`

`dev.ps1` lives at the repo root and wraps build + OTA-flash + monitor. Run from the repo root:

```powershell
.\dev.ps1                          # build + OTA-flash + monitor (main app)
.\dev.ps1 build                    # build only
.\dev.ps1 flash                    # build + OTA-flash (no monitor)
.\dev.ps1 flash-usb                # build + USB-flash to running slot
.\dev.ps1 monitor                  # idf.py monitor

.\dev.ps1 -App recovery build      # build the recovery app
.\dev.ps1 -App recovery flash-usb  # USB-flash recovery to the factory slot

.\dev.ps1 -ip 10.71.79.1 ...       # override target IP (default = SoftAP IP)
.\dev.ps1 -port COM7 ...           # override serial port (default COM3)
```

The OTA path posts to `http://10.71.79.1/api/ota/{upload-app,upload-ui,commit}` — your laptop must be associated to the `HERO-RC-XXXX` SoftAP first. OTA is main-app only (recovery has no `storage.bin`).

---

## First flash from blank — `flash_factory.ps1`

For a brand-new board, or after `python -m esptool erase_flash`, use the full provisioning script:

```powershell
.\tools\flash_factory.ps1
```

This builds both apps and writes everything over USB at correct offsets: bootloader, partition table, `ota_data_initial` (so the bootloader picks `ota_0` immediately), recovery into `factory`, main app into `ota_0`, and the LittleFS web UI into `storage`. `ota_1` is left blank for the first OTA to populate.

---

## Build manually (without dev.ps1)

Each app builds with `idf.py` like a normal ESP-IDF project:

```powershell
cd apps\main
idf.py build

cd ..\recovery
idf.py build
```

After any `sdkconfig.defaults` change, delete the live `sdkconfig` so the new defaults are picked up.

---

## Web UI development

Two separate UIs, one per app:

- **Main app:** [`apps/main/web_ui/`](../apps/main/web_ui/) — `index.html`, `style.css`, `app.js`, `updates.js`. Compressed by `compress.py` at build time and packed into the `storage` LittleFS partition. Served from `http://10.71.79.1/`.
- **Recovery app:** [`apps/recovery/main/recovery.html`](../apps/recovery/main/recovery.html) — single embedded HTML page (no LittleFS dependency), served from the same address when recovery is the running app.

After editing main-app web files:

```powershell
.\dev.ps1 flash    # picks up storage.bin changes via OTA
```

On first boot with a blank or corrupted storage partition, LittleFS formats automatically. The browser will show a placeholder page until the storage partition is flashed.

---

## Verifying a flash actually took

Every `idf.py build` updates the `build_date`/`build_time` fields exposed at `/api/version`, surfaced as a **Built** row in the Updates panel. If that row doesn't change after a flash, the new bytes didn't land — useful when chasing cache or partition issues.
