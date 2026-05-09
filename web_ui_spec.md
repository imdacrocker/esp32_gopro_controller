# Web UI Specification — GoPro CAN-BUS Controller

> **Baseline:** Current `index.html` (single-file, 1385 lines, ~54 KB).
> V2 will serve pre-gzipped files from LittleFS.  This document captures
> every detail of the present implementation so V2 can be rebuilt from scratch
> without consulting the original file.
>
> **CHANGE MARKERS:** Sections or items marked `<!-- TODO -->` are places where
> changes are expected or a decision is still open.

---

## 1. Delivery & Build

| Property | Current | V2 Target |
|---|---|---|
| File structure | Single `index.html` (inline CSS + JS) | Separate `index.html`, `app.css`, `app.js` pre-gzipped at build time |
| Served from | Embedded in firmware via `EMBED_TXTFILES` | LittleFS data partition (3 MB `storage` partition already in `partitions.csv`) |
| iOS caching fix | Not yet applied | Serve with explicit `Content-Length` header |
| Encoding | UTF-8 | UTF-8 |

The single-file format remains acceptable for V2 if the gzip step is handled by the build system; splitting is optional.

---

## 2. Layout & Viewport

```
max-width: 480px
margin: 0 auto
padding: 0 1em 5em   ← 5em bottom clearance for fixed bar
font-family: -apple-system, BlinkMacSystemFont, "Segoe UI", sans-serif
font-size (root): 20px   ← all rem values multiply from this
```

The page is intentionally narrow/mobile-first. It is usable on a phone held in one hand at the side of a race car.

### Vertical stacking order (top to bottom, in DOM order)

1. Sticky page header (gear icon)
2. RaceCapture Status section
3. Auto-Control section
4. `<p id="status">` — transient feedback line
5. Camera Status section (control bar + camera cards)
6. Fixed bottom bar ("Add / Manage Cameras" button)

---

## 3. Color Palette & Design Tokens

All values used verbatim in the current CSS:

```css
/* Blues — connected, buttons, links */
--blue:        #2980b9;
--blue-hover:  #2471a3;

/* Greens — recording, start button, auto-control ON, logging */
--green:       #27ae60;
--green-dark:  #2e7d32;   /* recording badge text only */
--green-hover: #219a52;   /* legacy add button hover */

/* Oranges */
--orange:      #f39c12;   /* not-recording badge, RC not-logging */
--orange-dark: #e67e22;   /* reboot button */
--orange-dark-hover: #ca6f1e;

/* Reds — stop, remove, reset, danger */
--red:         #e74c3c;
--red-hover:   #c0392b;
--red-dark-hover: same as red-hover

/* Grays */
--gray-text:   #888;      /* section titles, settings-btn icon */
--gray-light:  #999;      /* secondary labels, disconnected badge */
--gray-label:  #555;      /* UTC time, status feedback text */
--gray-bg:     #f8f8f8;   /* section title backgrounds */
--gray-border: #ddd;      /* most borders */
--gray-border-light: #eee; /* card separators */
--gray-disabled: #e0e0e0; /* disabled shutter button background */
--gray-disabled-text: #bbb;
--gray-toggle-off: #ccc;  /* toggle track when off */

/* Text */
--text-primary:   #222;
--text-secondary: #555;
--white: #fff;

/* Backgrounds */
--card-bg:  #fafafa;
--modal-bg: #fff;
--page-bg:  #fff;
--overlay:  rgba(0, 0, 0, 0.45);
```

<!-- CSS custom properties (`:root { --blue: … }`) should be adopted in V2 for maintainability. -->

---

## 4. Typography Scale

| Class / Element | font-size | font-weight | Notes |
|---|---|---|---|
| `h1` in header | `1.15rem` (23px) | 600 | letter-spacing 0.02em |
| Section titles (`.rc-section-title`, etc.) | `0.7rem` (14px) | 700 | UPPERCASE, letter-spacing 0.1em, color `#888` |
| `cam-number` | `0.95rem` | 600 | |
| `cam-display-name` | `0.7rem` | 700 | UPPERCASE, letter-spacing 0.06em, color `#999` |
| `cam-model-name` | `0.72rem` | 400 | color `#999` |
| Status badge | `0.88rem` | 600 | |
| RC value | `1.05rem` | 600 | |
| RC label | `0.68rem` | 400 | UPPERCASE, letter-spacing 0.08em, color `#999` |
| UTC date line | `0.72rem` | 600 | monospace, color `#888` |
| UTC time line | `0.88rem` | 600 | monospace, color `#555` |
| Auto-control label | `0.92rem` | 600 | color `#222` |
| Auto-control sub | `0.72rem` | 400 | color `#888`, line-height 1.4 |
| Toggle state text | `0.78rem` | 600 | min-width 26px |
| Buttons (primary) | `0.88rem` | 700 | |
| Buttons (control bar) | `0.85rem` | 700 | |
| Settings row label | `0.9rem` | 600 | |
| Settings select | `0.85rem` | 400 | |
| Modal title | `1rem` | 700 | |
| Modal section title | `0.65rem` | 700 | UPPERCASE, letter-spacing 0.1em, color `#999` |
| Paired cam name | `0.9rem` | 600 | |
| Paired cam meta | `0.72rem` | 400 | color `#999` |
| Found cam name | `0.88rem` | 600 | |
| Found cam addr | `0.72rem` | 400 | color `#999` |
| Type badge | `0.6rem` | 700 | UPPERCASE, letter-spacing 0.06em |
| Empty message | `0.88rem` – `0.9rem` | 400 | color `#888` |

---

## 5. Animations

```css
/* Pulsing dot on the recording status badge */
@keyframes cam-pulse {
    0%, 100% { opacity: 1; }
    50%       { opacity: 0.25; }
}
animation: cam-pulse 1.2s infinite;

/* Loading spinner (shown before first camera poll) */
@keyframes spin {
    to { transform: rotate(360deg); }
}
animation: spin 0.75s linear infinite;
/* Spinner: 28×28px, 3px border, border-color #e0e0e0, border-top-color #2980b9 */
```

---

## 6. Page Header

```
id: page-header
position: sticky, top: 0, z-index: 50
background: #fff
border-bottom: 1px solid #ddd
padding: 16px 20px
```

- **Title:** "GoPro Controller" — centered, `h1`, `1.15rem`, weight 600
- **Settings button** (`id="settings-btn"`): absolute, vertically centered, right edge (right: 16px)
  - Gear SVG icon, 22×22, stroke currentColor
  - Default color: `#888`; hover: `#333`, background `#f0f0f0`
  - Opens the Settings top-sheet modal

<!-- Title is "GoPro Controller" — update if desired. -->

---

## 7. RaceCapture Status Section

```
id: rc-status-section
border: 1px solid #ddd, border-radius: 10px, overflow: hidden
margin: 0.4em 0 0.8em
```

**Section title bar:** "RaceCapture Status", uppercase, `0.7rem`, weight 700, background `#f8f8f8`, border-bottom `#ddd`

**Two-cell grid** (`display: grid; grid-template-columns: 1fr 1fr`):

| Cell | Label | Value element | States |
|---|---|---|---|
| Left | "Logging" | `#rc-logging-pill` | See below |
| Right | "RC Date/Time (Local)" | `#utc-display` | See below |

**Logging pill classes** (applied to `#rc-logging-pill`):
```
rc-value rc-logging      → color: #27ae60   text: "Logging"
rc-value rc-not-logging  → color: #f39c12   text: "Not Logging"
rc-value rc-unknown      → color: #e74c3c   text: "Unknown"
```
CSS rule: `el.className = 'rc-value rc-' + state.replace('_', '-')`

**UTC / Local time display:**
- Two `<div>` children inside `#utc-display` (font-family: monospace)
- `#utc-date-line`: `YYYY-MM-DD`, `0.72rem`, color `#888`
- `#utc-time-line`: `HH:MM:SS`, `0.88rem`, color `#555`
- When GPS not yet valid: date line = "No GPS", time line = ""
- Firmware returns epoch_ms with timezone offset already applied; JS reads getUTC* methods

---

## 8. Auto-Control Section

```
id: auto-control-section
border: 1px solid #ddd, border-radius: 10px, overflow: hidden
margin: 0 0 0.8em
```

Single row (`#auto-control-row`, padding 14px, flex, space-between):

**Left side (text):**
- Label: "Automatic Control", `0.92rem`, weight 600, color `#222`
- Sub-label: "Cameras will start and stop recording automatically based on the RaceCapture logging status.", `0.72rem`, color `#888`

**Right side (toggle):**
- State label: "On" (green `#27ae60`) / "Off" (gray `#999`), `0.78rem`, weight 600
- Custom toggle: 56×32px track, 24px thumb
  - Track ON: background `#27ae60`; OFF: background `#ccc`
  - Thumb: always white, slides left→right (left: 4px → 28px)
  - Transition: `background 0.2s`, `left 0.2s`
- Clicking the entire `.toggle-wrap` calls `setAutoControl(!autoControlEnabled)`

**Effect on Camera Status section:**
- Auto ON → control bar (`#control-bar`) hidden; per-camera shutter buttons hidden
- Auto OFF → control bar shown (2-col grid); per-camera shutter buttons shown on connected cameras

---

## 9. Status Feedback Line

```html
<p id="status"></p>
```
- Color `#555`, `0.5em 0 1em` margin, min-height `1.2em`
- Transient text set by `setStatus(msg)` after shutter commands
- Cleared automatically only by subsequent commands (not on a timer)

---

## 10. Camera Status Section

```
id: cam-status-section
border: 1px solid #ddd, border-radius: 10px, overflow: hidden
margin: 0 0 0.8em
```

**Section title bar:** "Camera Status", same style as RaceCapture title bar

**Control bar** (`#control-bar`):
- Shown only when Auto-Control is OFF
- 2-column grid, gap 10px, padding 14px, border-bottom `#ddd`
- "Record All" button: green `#27ae60`, calls `sendShutter(true)`
- "Stop All" button: red `#e74c3c`, calls `sendShutter(false)`
- Both buttons: `0.85rem`, weight 700, min-height 48px, border-radius 8px
- Both disabled during in-flight request; re-enabled in `.finally()`

**Camera list** (`#cam-status-list`):

Initial state: shows spinner (`#cam-status-loading`) until first poll resolves.

Empty state: `<span id="cam-status-empty">No cameras paired.</span>`, color `#888`, `0.9rem`

**Camera card** (`.camera-card`):
```
padding: 16px 14px
border-bottom: 1px solid #eee
```

Card internal layout:
```
.cam-meta  (flex, align-items: baseline, gap: 8px, margin-bottom: 4px)
  .cam-number       "Cam {index}"    0.95rem, weight 600, #222
  .cam-display-name  {cam.name}       0.7rem,  weight 700, UPPERCASE, #999
  .cam-type-badge   "WiFi RC"        (RC-emulation cameras only — see §10.2)

.cam-model-name  {cam.model_name}  0.72rem, #999, margin-bottom 12px
  (omitted when model_name is absent or equals cam.name)

.cam-footer  (flex, align-items: center, justify-content: space-between, gap: 10px)
  .status-badge  + optional shutter button
```

### 10.1 Status Badge

```
.status-badge  display: inline-flex, align-items: center, gap: 8px
               font-weight: 600, font-size: 0.88rem
.status-dot    border-radius: 50%, flex-shrink: 0
```

| API `status` | CSS class | Dot | Text color | Label |
|---|---|---|---|---|
| `disconnected` | `disconnected` | hidden (`display: none`) | `#999` | "Not Connected" |
| `pairing` | `pairing` | 9×9px, `#2980b9`, `cam-pulse 1.2s` | `#2980b9` | "Pairing…" |
| `connecting` | `connecting` | 9×9px, `#2980b9`, `cam-pulse 1.2s` | `#2980b9` | "Connecting…" |
| `idle` | `idle` | 9×9px, `#f39c12` (solid) | `#f39c12` | "Idle" |
| `recording` | `recording` | 9×9px, `#27ae60`, `cam-pulse 1.2s` | `#2e7d32` | "Recording" |

**Status mapping** (from `camera_manager` enums; see `camera_manager_design.md` §...):
- `pairing` only applies to BLE cameras (initial add-camera flow before `first_pair_complete`).
- `connecting` applies to both transports. For BLE: any non-READY state once `first_pair_complete` is set. For RC: the slot is associated to the SoftAP but `wifi_status != WIFI_CAM_READY` — either still waiting on the first UDP response after a fresh associate, or demoted from READY by the keepalive silence watchdog (WoL retry loop active). The slot returns to `idle`/`recording` on the next received UDP datagram, or to `disconnected` if the camera disassociates from the AP.
- `idle` and `recording` are gated on `wifi_status == WIFI_CAM_READY` (which BLE drivers also flip at the end of their readiness sequence — the field is overloaded as the universal "fully ready" signal).

### 10.2 Type Badge (RC-emulation cameras)

```
.cam-type-badge
  font-size: 0.6rem, font-weight: 700, UPPERCASE, letter-spacing 0.06em
  padding: 2px 6px, border-radius: 4px
  background: #e8f4fd, color: #2980b9, border: 1px solid #aad4f0
```
Shown on camera cards and paired rows when `cam.type === 'rc_emulation'`. Badge text: "WiFi RC".

### 10.3 Per-Camera Shutter Buttons

Rendered only when **Auto-Control is OFF** and camera is `idle` or `recording`:

```
.cam-shutter-btn  border-radius: 8px, font-size: 0.88rem, font-weight: 700
                  padding: 11px 20px, min-height: 44px
.cam-shutter-start  background: #27ae60 (green)   text: "Record"
.cam-shutter-stop   background: #e74c3c (red)     text: "Stop"
disabled state:     background: #e0e0e0, color: #bbb
```

**Shutter lock (5s):** After tapping a per-camera button:
1. Button disabled immediately
2. `shutterLocked[slot] = { expectedStatus, timer }` — 5s timeout
3. Next `refreshCameraStatus()` checks if camera reached expected state; if so, clears lock early
4. Lock auto-expires after 5s regardless

Calls `POST /api/shutter` with `{ slot, on }`.

---

## 11. Fixed Bottom Bar

```
id: manage-bar
position: fixed, bottom: 0, left/right: 0
background: #fff, border-top: 1px solid #ddd
padding: 0.75em 1em
z-index: 100
```

Single button `#manage-btn`: "+ Add / Manage Cameras", blue `#2980b9`, full-width (max 480px), 48px min-height. Opens Manage Cameras bottom-sheet modal.

---

## 12. Settings Modal (Top Sheet)

Triggered by the gear icon in the page header.

```
id: settings-overlay
position: fixed, inset: 0
background: rgba(0,0,0,0.45)
z-index: 200
aligns: flex-start (sheet anchors to top)
```

**Sheet panel** (`.settings-modal`):
```
background: #fff
border-radius: 0 0 12px 12px   ← only bottom corners rounded
width: 100%, max-width: 480px
padding: 0 0 1.5em
```

**Header:** "Settings" title left, "Done" button right (blue, closes modal)

Clicking the overlay backdrop (not the modal card) also closes the modal.

**Section 1 — Device:**
- Title: "DEVICE", `0.65rem`, UPPERCASE, color `#999`
- Single row: label "Time Zone" left, `<select>` right
  - Select options: UTC-12 … UTC+14 (whole hours), populated by `buildTimezoneDropdown()` on page load
  - Labels: "UTC-12" … "UTC" … "UTC+14"
  - On change: `POST /api/settings/timezone` with `{ tz_offset_hours: int }`
  - On open: `GET /api/settings/timezone` → sets selected value
- **Set Date & Time row** — only rendered when `session_synced == false` (checked at modal open via `GET /api/utc`):
  - Label "Set Date & Time" left, `<button>` "Set from Device" right
  - Button style: blue outline, `0.85rem`, `min-height: 36px`, `border-radius: 6px`
  - On tap: reads `Date.now()` from the browser, `POST /api/settings/datetime` with `{ epoch_ms: number }`
  - On success: brief inline confirmation "Time set ✓" replaces button for 2 s, then restores
  - On error: inline "Failed — try again" in red for 2 s
  - Row is hidden (not just disabled) once a live source has set time this session, so it does not clutter the UI during normal race operation
  - Note: the firmware persists UTC across reboots, so `valid` may be `true` (NVS-restored) while `session_synced` is still `false`. The row must gate on `session_synced` — gating on `valid` would hide the row even when the only available time is a stale boot-time restore

**Reboot button:**
```
display: block, width: calc(100% - 32px), margin: 16px 16px 0
background: #e67e22 (orange), color: #fff
font-size: 0.88rem, font-weight: 700, min-height: 48px, border-radius: 8px
```
- Confirm dialog: "Reboot Controller?\n\nThe device will restart. Paired cameras and settings will be preserved."
- On confirm: `POST /api/reboot`, disable button, show "Rebooting…" with dot animation
- After 5s: `location.reload()`

**Factory Reset button:**
```
background: #e74c3c (red)
```
- Confirm dialog: "Restore Defaults?\n\nThis will erase all paired cameras and settings, then restart the controller. This cannot be undone."
- On confirm: `POST /api/factory-reset`, same animation pattern → `location.reload()` after 5s

---

## 13. Manage Cameras Modal (Bottom Sheet)

Triggered by "Add / Manage Cameras" button.

```
id: modal-overlay
position: fixed, inset: 0
background: rgba(0,0,0,0.45)
z-index: 200
aligns: flex-end (sheet anchors to bottom)
```

**Sheet panel** (`.modal`):
```
background: #fff
border-radius: 12px 12px 0 0   ← only top corners rounded
width: 100%, max-width: 480px
max-height: 85vh, overflow-y: auto
padding: 0 0 2em
```

**Header:** "Manage Cameras" title left, "Done" button right. Sticky (z-index 10) so it stays visible while scrolling.

On open: immediately calls `refreshModalPairedCameras()` and starts a 3s interval that refreshes the paired list and (when activated) the RC-discovered list. The RC-discovered list is **not** populated on modal open — it stays empty until the user clicks "Add a new Wifi RC Camera" (see §13.2). A module-level `rcListActivated` flag gates `refreshRcDiscovered()`; the periodic poll continues to call it but it short-circuits while the flag is false.

On close: cancels any active BLE scan, clears the 3s refresh interval, resets all modal UI state including `#rc-results` and `rcListActivated = false` (so reopening the modal again starts the RC section blank).

Clicking backdrop also closes modal.

**Action-row pattern (§13.1, §13.2):** Each "add a new camera" section is rendered as a `.modal-action-row` flex row containing:
- A primary `.modal-action-btn` (flex:1) — the action button
- A 44×44 px circular `.modal-info-btn` — info icon (Lucide-style "i in a circle" SVG, 28×28)
- An absolutely-positioned `.modal-info-tooltip` (top: row + 6 px, right: 0, max-width 280 px, dark background `#2a2a2a`, white text) — hidden by default

Tapping the info icon toggles the `.show` class on its sibling tooltip. Tapping a different info icon swaps which tooltip is visible (only one open at a time). Tapping anywhere outside an info icon or open tooltip closes all open tooltips.

Below each action row is a section caption styled as `.modal-section-title` with `padding-top:0` (so it reads as a caption *for* the button above, not a heading for what follows). Each section ends with a `.modal-separator` (1 px `--gray-border-light` line, 12 px vertical / 16 px horizontal margin).

### 13.1 Section 1 — Pair a New Camera (BLE)

**Action button** (`#scan-btn`):
- Idle: blue `#2980b9`, text "Add a new Bluetooth camera"
- Scanning: background `#f0f0f0`, border `1px solid #ccc`, color `#333`, text "Cancel Scan"
- Toggle behavior: if scanning → cancel; else → start

**Info-icon tooltip:** "Put your camera into Pairing mode, like you would for the GoPro Quik App"

**Caption (below button):** "Pair a Hero9 Black or newer over BLE"

**Scan flow:**
1. `POST /api/scan` → start 1s poll on `GET /api/cameras` + `GET /api/paired-cameras`
2. Show countdown: "Scanning… 120s" decrementing every second
3. Filter discovered list to exclude already-paired addresses
4. Render unpaired cameras in `#results` as `.found-camera-row` cards:
   - Name (`0.88rem`, weight 600) + address + RSSI (`0.72rem`, `#999`)
   - "Pair" button (blue, `pair-this-btn`)
5. Auto-stop at 120s (121s timer to account for last second display); final poll on stop
6. Status line (`#modal-status`): shows countdown / "Scan complete." / "Scan cancelled." / errors

**Pairing:**
- Click "Pair" → cancel active scan → `POST /api/pair` with `{ addr, addr_type }`
- Status: "Pairing initiated — camera should appear in the list shortly."
- Result list cleared; new camera will appear on next camera status poll

### 13.2 Section 2 — Add WiFi RC Emulation Camera

**Action button** (`#rc-add-btn`): green `#27ae60`, text "Add a new Wifi RC Camera"

**Info-icon tooltip:** "On your camera, add a new connection, and choose either the Wifi RC or Smart Remote option. The camera will automatically pair successfully. Then you can add it to the controller here."

**Caption (below button):** "Pair an older camera as a Wifi Remote"

**Activation:** `#rc-results` stays empty when the modal opens. The first click of `#rc-add-btn` sets `rcListActivated = true` and calls `refreshRcDiscovered()`; from that point the existing 3s modal poll keeps the list current. The flag resets on modal close.

On click (and on each subsequent poll while activated): `GET /api/rc/discovered`
- Returns array of `{ addr, ip }` for SoftAP-connected stations whose MAC OUI is on the GoPro allow-list (see `GOPRO_RC_OUIS[]` in `api_rc.c` — IEEE MA-L registrations to Woodman Labs / GoPro) and that aren't already in a managed RC slot. Non-GoPro stations (phones, laptops viewing the web UI) are filtered out server-side.
- 0 results: "No unidentified devices connected."
- N results: "{N} device(s) connected — click Add to probe:"

Each unidentified device renders as a `.found-camera-row`:
- Name: "Unknown Device"
- Meta: `{addr} — {ip}` (or "IP pending" if no IP)
- "Add" button (blue, `pair-this-btn`)

**Add flow** (mirrors the BLE pair flow — same `pair-overlay` modal and `/api/pair/status` polling):
1. If IP missing: show "Cannot add — IP address not yet assigned. Wait a moment and click Add a new Wifi RC Camera again." and abort
2. Open the pair-progress modal (`openPairModal()`); this also closes the Manage Cameras modal so the home screen takes focus on success
3. `POST /api/rc/add` with `{ addr, ip }` — firmware reserves the shared pair-attempt machine (`PAIR_TRANSPORT_WIFI_RC`) and primes the camera with keepalive + `st` + `cv`. `409 Conflict` means another pair attempt is in flight; the modal flips to the failure view immediately.
4. `startPairStatusPoll()` — UI polls `GET /api/pair/status` every 1 s.
   - State `connecting` → "Connecting to camera…" (default label)
   - State `success` → "Success!", auto-dismiss after 2 s, then `refreshCameraStatus()` repaints the home-screen camera list. The cv-resolved model name surfaces on the home-screen card on its next refresh, even if cv arrived after the modal closed.
   - State `failed` → mapped error label (e.g. `handshake_timeout` → "Camera setup timed out. Try again."); button changes to "OK".
5. Cancel button → `POST /api/pair/cancel`. Server-side this removes the just-registered slot (RC slots are committed at register time; see §9.1 of `camera_manager_design.md`).

### 13.3 Section 3 — Paired Cameras

Title: "PAIRED CAMERAS" + count badge (hidden when 0). Unlike the §13.1/§13.2 captions (which use the default `--gray-light` colour), this title is rendered in `--text-primary` (dark) via an inline `style="color:var(--text-primary)"` override on the `.modal-section-title` span, to mark it as a heading for the list rather than a caption for a button above.

Source: `GET /api/paired-cameras`

Each camera renders as a `.modal-paired-row` (background `#fafafa`, border `#eee`, border-radius 8px):
- Left: name line (`0.9rem`, weight 600) with optional type badge (shown when `cam.type === 'rc_emulation'`, text "WiFi RC"); meta line (`0.72rem`, `#999`) showing model_name · Cam {index} [· addr if RC-emulation]
- Right: "Remove" button in red `#e74c3c`

**Remove flow:** Confirm dialog → `POST /api/remove-camera` with `{ slot }` → refresh lists. RC-emulation removal also waits 1.5s then refreshes `GET /api/rc/discovered` (async slot free).

---

## 14. Polling Summary

| Timer | Interval | Endpoints | Purpose |
|---|---|---|---|
| Camera status | 3s | `GET /api/paired-cameras` | Camera cards on main screen |
| RC status + UTC + auto-control | 2s | `GET /api/logging-state`, `GET /api/utc`, `GET /api/auto-control` | Top two sections |
| BLE scan results | 1s (during scan only) | `GET /api/cameras`, `GET /api/paired-cameras` | Found cameras list in modal |
| Modal refresh | 3s (modal open only) | `GET /api/paired-cameras`, `GET /api/rc/discovered` | Paired list + RC-emulation list in modal |

All polls fire independently via `setInterval`; no coordination or debouncing between timers.

---

## 15. HTTP API Contract (consumed by this UI)

| Method | Path | Request body | Response body | Notes |
|---|---|---|---|---|
| GET | `/api/logging-state` | — | `{ state: "logging"\|"not_logging"\|"unknown" }` | |
| GET | `/api/utc` | — | `{ valid: bool, session_synced: bool, epoch_ms: int }` | epoch_ms has tz offset applied. `valid` is true whenever any anchor is available (incl. NVS-restored at boot). `session_synced` is true only after a live source — GPS frame or manual web set — won this boot session. |
| GET | `/api/auto-control` | — | `{ enabled: bool }` | |
| POST | `/api/auto-control` | `{ enabled: bool }` | `{ enabled: bool }` | |
| GET | `/api/cameras` | — | `[{ name, addr, addr_type, rssi }]` | BLE scan results |
| GET | `/api/paired-cameras` | — | `[{ slot, index, name, model_name, type, addr, status }]` | `slot` and `index` are **1-based** — first paired camera is `1`. `type`: `"ble"` or `"rc_emulation"`. `status`: WiFi cameras → `"disconnected"\|"connecting"\|"idle"\|"recording"`; BLE cameras → `"disconnected"\|"pairing"\|"connecting"\|"idle"\|"recording"`. For RC, `connecting` covers both the post-associate window before the first UDP response and the keepalive-silence WoL-retry loop |
| POST | `/api/scan` | — | `{}` | Starts BLE scan |
| POST | `/api/scan-cancel` | — | `{}` | Cancels BLE scan |
| POST | `/api/pair` | `{ addr, addr_type }` | `{}` | Initiates BLE pairing. Returns `409 Conflict` if a previous pair attempt is still in flight (UI polls `/api/pair/status` until terminal). |
| POST | `/api/pair/cancel` | — | `{}` | Aborts the in-flight pair attempt (BLE *or* RC). For BLE, calls `ble_gap_conn_cancel` / `ble_gap_terminate`. For RC, removes the just-registered slot. Idempotent. |
| GET | `/api/pair/status` | — | `{ state, transport, addr, addr_type, model, model_name, error_code, error_message }` | Shared by BLE and RC Add flows. `state`: `"idle"\|"connecting"\|"bonding"\|"provisioning"\|"success"\|"failed"` (RC never reports `bonding`/`provisioning` — it goes `connecting` → `success`). `transport`: `"ble"` or `"wifi_rc"`. Sticky terminal state — `success` and `failed` persist until the next `POST /api/pair` or `POST /api/rc/add`. `addr_type` is BLE-only (0 for RC). `error_code`: `"none"\|"slots_full"\|"ble_connect_failed"\|"bond_failed"\|"hwinfo_timeout"\|"model_unsupported"\|"handshake_timeout"\|"disconnected"\|"cancelled"\|"internal"`. |
| POST | `/api/remove-camera` | `{ slot }` | `{}` | Removes paired camera (both types). `slot` is **1-based**. |
| POST | `/api/shutter` | `{ on: bool }` or `{ slot, on: bool }` | `{ dispatched: int }` | Omit `slot` for all cameras. `slot` is **1-based**. |
| GET | `/api/rc/discovered` | — | `[{ addr, ip }]` | Unprobed SoftAP stations whose MAC OUI is on the GoPro allow-list (`GOPRO_RC_OUIS[]` in `api_rc.c`). Non-GoPro stations are filtered out server-side. |
| POST | `/api/rc/add` | `{ addr, ip }` | `{}` | Reserves the shared pair-attempt machine (`PAIR_TRANSPORT_WIFI_RC`), then registers the slot and primes UDP keepalive + `st` + `cv`. UI polls `/api/pair/status` for `success` / `failed`. Returns `409 Conflict` if a pair attempt is already in flight. |
| POST | `/api/reboot` | — | `{}` or no response | ESP32 may drop connection before responding |
| POST | `/api/factory-reset` | — | `{}` or no response | Same as above |
| GET | `/api/settings/timezone` | — | `{ tz_offset_hours: int }` | |
| POST | `/api/settings/timezone` | `{ tz_offset_hours: int }` | `{}` | |
| POST | `/api/settings/datetime` | `{ epoch_ms: number }` | `{}` | Only accepted when `session_synced == false` (no live source has won yet — NVS-restored boot value does not block manual entry). Sets system time from browser clock; triggers `open_gopro_ble_sync_time_all()` and `gopro_wifi_rc_sync_time_all()`. |

---

## 16. JavaScript State Variables

| Variable | Type | Initial value | Purpose |
|---|---|---|---|
| `autoControlEnabled` | bool | `true` | Mirrors firmware auto-control flag |
| `shutterLocked` | object | `{}` | Per-slot lockout: `{ expectedStatus, timer }` |
| `scanning` | bool | `false` | BLE scan in progress |
| `cameraStatusLoaded` | bool | `false` | Suppresses spinner after first poll |
| `pollTimer` | interval ID | `null` | 1s BLE scan poll |
| `countdownTimer` | interval ID | `null` | 1s scan countdown display |
| `modalPairedRefreshTimer` | interval ID | `null` | 3s modal refresh |

---

## 17. Open Items / Known V2 Decisions

- **Camera type field:** V2 API uses `"rc_emulation"` for Hero 4 and `"ble"` for Hero 9+ (not `"legacy_wifi"` / `"cohn"`). UI badge text is "WiFi RC" / "BLE". ✅ Resolved.
- **Model selection for RC-emulation cameras:** Firmware defaults to `HERO4_BLACK`; no model picker in the UI. ✅ Resolved (deferred).
- **BLE-control cameras (Hero 9+):** Pairing alone is sufficient — there is no separate provisioning step. The post-readiness sequence (`GetHardwareInfo` → `SetCameraControlStatus(EXTERNAL)` → `SetDateTime` → status poll) runs automatically inside `open_gopro_ble`. ✅ Resolved.
- **Status granularity:** Five states for BLE cameras (`disconnected` / `pairing` / `connecting` / `idle` / `recording`); four for WiFi (`disconnected` / `connecting` / `idle` / `recording`). The `pairing` vs `connecting` distinction (BLE only) is driven by the persisted `first_pair_complete` flag. RC `connecting` covers any time the slot is associated to the SoftAP but `wifi_status != WIFI_CAM_READY` — including the keepalive-silence WoL-retry window.
- **Pair-attempt polling:** Both the BLE Add flow and the WiFi RC Add flow drive the same `pair-overlay` modal and poll `GET /api/pair/status` every ~1 s until the state reaches `success` or `failed`. Errors are mapped to user-friendly messages by `PAIR_ERROR_LABEL` in `app.js`. The `model_unsupported` code surfaces frozen models (e.g. Hero 7) so the user gets a clear "this model is not supported" message instead of a silent disconnect. RC failures show as `handshake_timeout` (no UDP response in 20 s) or `slots_full`. The 15-second arbitrary refresh delay used in the pre-shared-modal RC flow is gone.
- **Color palette:** V2 should use CSS custom properties (`:root { --blue: #2980b9; … }`) for maintainability.
- **Settings → Device section:** May need additional entries (e.g. per-camera name edit) — TBD.
- **Timezone half-hours:** Whole-hour offsets only. Not a V2 priority (e.g. UTC+5:30 would need `float` or half-hour `int`).
