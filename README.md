# ESP32 GoPro CAN-Bus Controller

A firmware for the Autosport Labs ESP32-CAN-X2 that allows a RaceCapture trigger multiple GoPro cameras.

When the RaceCapture starts logging, your GoPros start recording. When it stops, they stop. Up to **four cameras at once**, paired over BLE (Hero9 black+) or WiFi (Hero9 Silver and older), with a phone-friendly web UI for setup and over-the-air updates.

---

## Quick start

For a brand-new board (one-time provisioning):

1. Connect your ESP32-CAN-X2 to your computer via USB-C.
2. Open ESP Launchpad pre-configured for this firmware (Chromium-based browser):

   <a href="https://espressif.github.io/esp-launchpad/?flashConfigURL=https://firmware-proxy.imdacrocker.workers.dev/launchpad.toml">
     <img alt="Try it with ESP Launchpad" src="https://espressif.github.io/esp-launchpad/assets/try_with_launchpad.png" width="250" height="70">
   </a>

3. Click **Connect** and pick the serial port the board enumerated on.
4. Click **Flash** — the merged factory image (bootloader + partition table + recovery + main app + UI) is written in one pass.
5. When flashing finishes, power-cycle the board.

That's it for USB. The board boots straight into the main app.

Then on your phone or laptop:

1. Join the WiFi network **`HERO-RC-XXXXXX`** (open, no password — last 3 MAC bytes in the SSID).
2. The controller UI should pop up automatically (captive portal). If it doesn't, open **<http://control.gp/>** in a browser — or **<http://10.71.79.1/>** if your device can't resolve the name.
3. Set your UTC offset from the settings menu at the top right.
4. Add cameras from the Add/Manage cameras menu at the bottom.

> **No internet on the flashing machine?** Download `factory.bin` from the [latest release](https://github.com/imdacrocker/esp32_gopro_controller/releases/latest) and use Launchpad's **DIY** tab to flash it at address `0x0`.

To send information from the RaceCapture, you will need to use Lua.  Here is an example script (the IDs `0x600` and `0x602` below are the factory defaults — if you've changed them in **Settings → CAN-BUS Settings** on the web UI, update the script to match):

```lua
local function isLeapYear(year)
    return (year % 4 == 0 and year % 100 ~= 0) or (year % 400 == 0)
end

-- Days per month lookup (non-leap year)
local DAYS_IN_MONTH = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31}

local function daysInMonth(month, year)
    if month == 2 and isLeapYear(year) then
        return 29
    end
    return DAYS_IN_MONTH[month]
end

local function dateToEpochMs(year, month, day, hour, minute, second, ms)
    -- Accumulate full years since epoch
    local days = 0
    for y = 1970, year - 1 do
        days = days + (isLeapYear(y) and 366 or 365)
    end

    -- Accumulate full months in the current year
    for m = 1, month - 1 do
        days = days + daysInMonth(m, year)
    end

    -- Add remaining days in current month (day is 1-based)
    days = days + (day - 1)

    -- Convert everything to milliseconds and add sub-second component
    local totalMs = ((days * 86400) + (hour * 3600) + (minute * 60) + second) * 1000 + ms
    return totalMs
end

local function packUint64LE(val)
    local b = {}
    for i = 1, 8 do
        b[i] = math.floor(val % 256)
        val  = math.floor(val / 256)
    end
    return b
end

function sendUtc()
    local year, month, day, hour, minute, second, ms = getDateTime()

    if year <= 1970 then
        return
    end

    local epochMs = dateToEpochMs(year, month, day, hour, minute, second, ms)
    local data    = packUint64LE(epochMs)

    txCAN(0, 0x602, 0, data)
end

setTickRate(50)
function onTick()
    sendUtc()
    txCAN(0, 0x600, 0, {isLogging(), 0, 0, 0, 0, 0, 0, 0}) -- Send the isLogging
end
```

Full instructions, manual builds, and recovery flows are in [`docs/development.md`](docs/development.md).

---

## Features

- **Four GoPros in parallel** — mix any camera combination from Hero2 (with Wifi BackPack) up to a Her13 black.
- **CAN-bus driven** — Reads RaceCapture logging state. Auto-start/auto-stop recording follows the vehicle.
- **Updates Camera Date/Time** - 
- **Mobile web UI** — Pair cameras, see status, change settings, install updates from any phone connected to the device's SoftAP. No cloud account.
- **Over-the-air updates** — Stable and beta channel updates, with automatic update checks (for devices with multiple connection capability, eg cellular internet and Wifi to the ESP32)
- **Recovery partition** — Always-present factory app catches any failed update and exposes a manual-upload web UI as a last resort.

---

## How it works

The device runs a SoftAP at `10.71.79.1`. You join it from a phone or laptop and open the web UI. Cameras pair to the device (BLE bond or WiFi association), then it relays start/stop commands triggered by the CAN bus.

```
   ┌───────────────────────────┐
   │  Vehicle CAN bus          │
   │  (RaceCapture logging)    │
   └─────────────┬─────────────┘
                 │
         ┌───────▼────────┐
         │   ESP32-S3     │      ──► up to 4 GoPros
         │  (this device) │            BLE  / WiFi RC
         └───────┬────────┘
                 │ SoftAP 10.71.79.1
         ┌───────▼────────┐
         │  Phone browser │
         │  (setup + OTA) │
         └────────────────┘
```

Full architecture, components, boot order, core affinity, and radio coexistence are in [`docs/architecture.md`](docs/architecture.md).

---

## Quick start

For a brand-new board (one-time provisioning):

1. Connect your ESP32-CAN-X2 to your computer via USB-C.
2. Open ESP Launchpad pre-configured for this firmware (Chromium-based browser):

   <a href="https://espressif.github.io/esp-launchpad/?flashConfigURL=https://firmware-proxy.imdacrocker.workers.dev/launchpad.toml">
     <img alt="Try it with ESP Launchpad" src="https://espressif.github.io/esp-launchpad/assets/try_with_launchpad.png" width="250" height="70">
   </a>

3. Click **Connect** and pick the serial port the board enumerated on.
4. Click **Flash** — the merged factory image (bootloader + partition table + recovery + main app + UI) is written in one pass.
5. When flashing finishes, power-cycle the board.

That's it for USB. The board boots straight into the main app.

Then on your phone or laptop:

1. Join the WiFi network **`HERO-RC-XXXXXX`** (open, no password — last 3 MAC bytes in the SSID).
2. The controller UI should pop up automatically (captive portal). If it doesn't, open **<http://control.gp/>** in a browser — or **<http://10.71.79.1/>** if your device can't resolve the name.
3. Set your UTC offset from the settings menu at the top right.
4. Add cameras from the Add/Manage cameras menu at the bottom.


> **No internet on the flashing machine?** Download `factory.bin` from the [latest release](https://github.com/imdacrocker/esp32_gopro_controller/releases/latest) and use Launchpad's **DIY** tab to flash it at address `0x0`.
>

To set up your RaceCapture, you must use Lua to send the isLogging and the UTC.  Here is an example of a full Lua code:

```lua
local function isLeapYear(year)
    return (year % 4 == 0 and year % 100 ~= 0) or (year % 400 == 0)
end

-- Days per month lookup (non-leap year)
local DAYS_IN_MONTH = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31}

local function daysInMonth(month, year)
    if month == 2 and isLeapYear(year) then
        return 29
    end
    return DAYS_IN_MONTH[month]
end

local function dateToEpochMs(year, month, day, hour, minute, second, ms)
    -- Accumulate full years since epoch
    local days = 0
    for y = 1970, year - 1 do
        days = days + (isLeapYear(y) and 366 or 365)
    end

    -- Accumulate full months in the current year
    for m = 1, month - 1 do
        days = days + daysInMonth(m, year)
    end

    -- Add remaining days in current month (day is 1-based)
    days = days + (day - 1)

    -- Convert everything to milliseconds and add sub-second component
    local totalMs = ((days * 86400) + (hour * 3600) + (minute * 60) + second) * 1000 + ms
    return totalMs
end

local function packUint64LE(val)
    local b = {}
    for i = 1, 8 do
        b[i] = math.floor(val % 256)
        val  = math.floor(val / 256)
    end
    return b
end

function sendUtc()
    local year, month, day, hour, minute, second, ms = getDateTime()

    if year <= 1970 then
        return
    end

    local epochMs = dateToEpochMs(year, month, day, hour, minute, second, ms)
    local data    = packUint64LE(epochMs)

    txCAN(0, 0x602, 0, data)
end

setTickRate(50)
function onTick()
    sendUtc()
    txCAN(0, 0x600, 0, {isLogging(), 0, 0, 0, 0, 0, 0, 0}) -- Send the isLogging
end
```


Full instructions, manual builds, and recovery flows are in [`docs/development.md`](docs/development.md).

---

## Repo map

```
esp32_gopro_controller/
├── apps/
│   ├── main/         — primary firmware (Hero control, web UI, CAN, OTA)
│   └── recovery/     — recovery firmware (factory partition, manual reflash UI)
├── components/       — shared between both apps (wifi_manager, ota_io)
├── tools/            — Cloudflare Worker + release manifest helper
├── docs/             — see Documentation below
├── dev.ps1           — daily-dev wrapper
└── partitions.csv    — shared 8 MB layout
```

Component-level READMEs live alongside their source: [`wifi_manager`](components/wifi_manager/README.md), [`ble_core`](apps/main/components/ble_core/README.md), [`open_gopro_ble`](apps/main/components/gopro/open_gopro_ble/README.md), [`gopro_wifi_rc`](apps/main/components/gopro/gopro_wifi_rc/README.md), [`can_manager`](apps/main/components/can_manager/README.md), [`camera_manager`](apps/main/components/camera_manager/README.md).

---

## Documentation

| Page | What's in it |
|------|--------------|
| [`docs/architecture.md`](docs/architecture.md) | Repo layout, component graph, boot sequence, core affinity, network, radio coex |
| [`docs/hardware.md`](docs/hardware.md) | SoC, flash, 8 MB partition layout |
| [`docs/development.md`](docs/development.md) | ESP-IDF setup, `dev.ps1`, `flash_factory.ps1`, manual builds, web UI dev loop |
| [`docs/releases.md`](docs/releases.md) | Channel model, cutting and promoting releases |
| [`docs/http-api.md`](docs/http-api.md) | Endpoint reference for the web UI's `/api/*` |
| [`docs/design/camera-manager.md`](docs/design/camera-manager.md) | Camera lifecycle, driver vtable, slot persistence |
| [`docs/design/ota.md`](docs/design/ota.md) | OTA system design — partition flow, manifest schema, Worker behavior |
| [`docs/design/web-ui.md`](docs/design/web-ui.md) | UI specification + full HTTP request/response contracts |
| [`CHANGELOG.md`](CHANGELOG.md) | Release-by-release user-facing changes |

---

## License

See [LICENSE](LICENSE).
