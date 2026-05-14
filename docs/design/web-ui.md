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

### Global CSS resets

Two non-default rules apply to every element on the page:

- `*, *::before, *::after { box-sizing: border-box; margin: 0; padding: 0 }` — standard reset.
- `[hidden] { display: none !important }` — forces the HTML `hidden` attribute to win over explicit `display:` rules. Required because some classes (`.modal-info-btn` for example) set `display: flex`, which would otherwise override the user-agent default `display: none` for `[hidden]` elements and silently break visibility toggles in JS.
- `body { touch-action: manipulation }` — disables iOS double-tap-to-zoom (and equivalent gestures on Android) while preserving pinch-zoom for accessibility. No `user-scalable=no` in the viewport meta — that would block pinch-zoom too.

### Vertical stacking order (top to bottom, in DOM order)

1. Sticky page header (gear icon)
2. RaceCapture section
3. System Time section
4. `<p id="status">` — transient feedback line (collapses to zero height when empty)
5. Cameras section (Auto-Control row + control bar + camera cards)
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
| Status badge | `0.88rem` | 600 | shared by the RC pill and camera cards |
| UTC date line | `0.88rem` | 600 | monospace, color `--gray-label` (`#555`); orange when `#utc-display.stale` |
| UTC time line | `0.88rem` | 600 | monospace, color `--gray-label` (`#555`); orange when `#utc-display.stale` |
| Section title btn (Sync) | `0.7rem` | 700 | UPPERCASE, letter-spacing 0.1em, blue outlined |
| Auto-control label | `0.92rem` | 600 | color `#222` |
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

## 7. RaceCapture Section

```
id: rc-status-section
border: 1px solid #ddd, border-radius: 10px, overflow: hidden
margin: 0.4em 0 0.8em
```

**Section title bar:** "RaceCapture", uppercase, `0.7rem`, weight 700, background `#f8f8f8`, border-bottom `#ddd`

Single cell (`.rc-cell`, padding `12px 14px`) containing only the logging pill `#rc-logging-pill`. The card no longer includes the date/time — see §8.

**Logging pill** uses the same `.status-badge` system as the camera cards in the Cameras section (§10.2). The pill is rendered as an icon + label; `pill.className = 'status-badge ' + camState`, where the API state is mapped via `RC_CAM_STATE`:

| API `state` | UI label | `.status-badge` class | Icon | Text colour |
|---|---|---|---|---|
| `logging` | "Logging" | `recording` | filled red dot, `cam-pulse 1.2s` | `#c0271f` |
| `not_logging` | "Not Logging" | `idle` | empty circle outline | `#248a3d` |
| `unknown` | "Disconnected" | `disconnected` | circle with diagonal slash | `#9aa0a6` |

The label-mapping table is `RC_LABEL` in `app.js`; the colour/icon comes from the shared `.status-badge` rules in `style.css`. The legacy `.rc-value` / `.rc-logging` / `.rc-not-logging` / `.rc-unknown` rules are gone — only `.status-badge` styles apply now.

Note that the firmware's `/api/logging-state` response still returns the literal `unknown`; only the rendered label changed to "Disconnected".

---

## 8. System Time Section

```
id: system-time-section
border: 1px solid #ddd, border-radius: 10px, overflow: visible   ← visible so the info-button tooltip can extend below the card
margin: 0 0 0.8em
```

**Section title bar:** flex row (`.section-title .section-title-row`) with the title on the left and a Sync button on the right.
- Left: `<span id="system-time-label">System Time (UTC)</span>`. The label is rewritten on startup (and whenever the user changes the dropdown in Settings) from `/api/settings/timezone`, formatted as `UTC`, `UTC+N`, or `UTC-N`. Fresh devices default to `UTC` (offset 0).
- Right: `<button id="datetime-btn" class="section-title-btn" hidden>Sync</button>` — small outlined blue button matching `--blue`. Hidden by default; revealed only when the system time still needs a manual sync (see "Sync gating" below).

**Body cell** (`.rc-cell`, padding `12px 14px`) contains `#utc-display` — a flex row laid out as:

```
#utc-display  (display:flex, align-items:center, gap:10px, position:relative)
  #utc-text   (display:flex, flex-direction:column, gap:2px)
    #utc-date-line   monospace 0.88rem weight 600 colour --gray-label
    #utc-time-line   monospace 0.88rem weight 600 colour --gray-label
  #time-info-btn     (.modal-info-btn, hidden by default; margin-left:auto)
  .modal-info-tooltip
```

Date and time render on **separate lines** with the same monospace styling. Date format is `Month D, YYYY` (e.g. `May 9, 2026`), built from a `MONTH_NAMES` array using `getUTCMonth() / getUTCDate() / getUTCFullYear()`. Time format is 12-hour with AM/PM (`hh:mm:ss AM/PM`, hours zero-padded), still using the UTC accessors. Firmware returns `epoch_ms` with the timezone offset already applied — JS uses the `getUTC*` methods so it does not re-apply the local timezone.

**Display states** (driven by `/api/utc` flags `valid` and `session_synced`):

| Condition | Date line | Time line | `#utc-display` class | Tooltip text |
|---|---|---|---|---|
| `!valid` (unknown) | empty | `--:--:--` | `.stale` (orange) | "No time synced. Either wait for the RaceCapture to send UTC, or manually sync from this device." |
| `valid && !session_synced` (NVS-restored / stale) | formatted date | formatted time | `.stale` (orange) | "System time was restored from memory but is stale and needs to be updated. Either wait for the RaceCapture to send UTC, or manually sync from this device." |
| `valid && session_synced` (live source won this boot) | formatted date | formatted time | (no `.stale`; default colour `--gray-label`) | (info button hidden, tooltip not shown) |

The `.stale` class colours both `#utc-date-line` and `#utc-time-line` with `var(--orange)` (`#f39c12`). There is no `(STALE)` suffix any more — colour is the only stale indicator.

**Sync gating:** the Sync button (`#datetime-btn`) and the info button (`#time-info-btn`) share the same visibility rule — both are hidden when `session_synced == true` and shown when `session_synced == false`. The `!syncBtn.dataset.busy` guard keeps the buttons stable while a manual sync is animating its "Set ✓" confirmation.

**Manual sync flow** (clicking `#datetime-btn`):
1. `dataset.busy = '1'`, button disabled, label `…`.
2. `POST /api/settings/datetime { epoch_ms: Date.now() }`.
3. On success: label flips to `Set ✓`, the `.stale` class is removed immediately, the info button is hidden, and after **3 seconds** the button hides itself and the busy flag clears.
4. On error: label flips to `Failed` for 3 seconds, then resets to `Sync` (still visible).

**Info button** (`#time-info-btn`) uses the same default `.modal-info-btn` styling as the buttons in the Add/Manage Cameras modal — 44×44 px circular with a 28×28 px "i in a circle" SVG. Its tooltip is the absolutely-positioned `.modal-info-tooltip` immediately following it inside `#utc-display`. The tooltip text is rewritten on each `/api/utc` poll to match the current state (see table above).

---

## 9. Status Feedback Line

```html
<p id="status"></p>
```
- Color `#555`, `0.5em 0 1em` margin, min-height `1.2em`
- Collapses to zero height (`#status:empty { margin: 0; min-height: 0 }`) when no message is set, so it does not introduce a phantom gap between the System Time and Cameras cards
- Transient text set by `setStatus(msg)` after shutter commands
- Cleared automatically only by subsequent commands (not on a timer)

---

## 10. Cameras Section

```
id: cam-status-section
border: 1px solid #ddd, border-radius: 10px, overflow: visible   ← visible so the auto-control info-button tooltip can extend below the card
margin: 0 0 0.8em
```

**Section title bar:** "Cameras", same style as the RaceCapture title bar.

### 10.0 Auto-Control row (first row inside the card)

The Auto-Control no longer has its own card — it lives as the first row inside the Cameras section, above the control bar.

```
#auto-control-row  (position:relative, display:flex, align-items:center,
                    padding:12px 14px, gap:14px, border-bottom:1px solid #ddd)
```

Row contents, left-to-right:
- `.ac-label` "Automatic Control" (`0.92rem`, weight 600, colour `--text-primary`).
- `.toggle-wrap` (`#toggle-wrap`) — the custom toggle, with the on/off state label rendered to the **right** of the toggle (not above it).
  - `.toggle-track` (56×32 px, border-radius 16, background `--gray-toggle-off`; `.on` swaps to `--green`).
  - `.toggle-thumb` (24×24 white, slides `left: 4px → 28px`).
  - `.toggle-state` (`#toggle-state`) text: "On" (colour `--green`) / "Off" (colour `--gray-light`), `0.78rem` weight 600.
  - Clicking anywhere in the wrap calls `setAutoControl(!autoControlEnabled)`.
- `#auto-control-info-btn` (`.modal-info-btn`, `margin-left:auto` so it right-justifies). Tapping toggles its sibling `.modal-info-tooltip`, which contains: "Turn this off for manual shutter control. Cameras will start and stop recording automatically based on the RaceCapture logging status when on."

The previous standalone descriptive paragraph (`.ac-sub`) is gone — that text now lives entirely inside the info-button tooltip.

**Effect on the rest of the section:**
- Auto ON → control bar (`#control-bar`) hidden; per-camera shutter buttons hidden.
- Auto OFF → control bar shown (2-col grid); per-camera shutter buttons shown on connected cameras.

### 10.1 Control bar

`#control-bar`:
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
  .cam-type-badge   "WiFi RC"        (RC-emulation cameras only — see §10.3)

.cam-model-name  {cam.model_name}  0.72rem, #999, margin-bottom 12px
  (omitted when model_name is absent or equals cam.name)

.cam-footer  (flex, align-items: center, justify-content: space-between, gap: 10px)
  .status-badge  + optional shutter button
```

### 10.2 Status Badge

```
.status-badge  display: inline-flex, align-items: center, gap: 8px
               font-weight: 600, font-size: 0.88rem
.status-icon   width/height: 14px, flex-shrink: 0
               (inline-flex container for the inner SVG or spinner div)
```

The badge is built by `makeBadge(status)` from a fixed `STATUS_ICON` table (SVG or empty `<span>` for the spinner) and a `STATUS_LABEL` table (`renderCameraCards` in `app.js`). The same code path renders the RaceCapture pill — see §7.

| API `status` | CSS class | Icon | Text color | Label |
|---|---|---|---|---|
| `disconnected` | `disconnected` | 14×14 SVG: outline circle with diagonal slash | `#9aa0a6` | "Not Connected" |
| `pairing` | `pairing` | 13×13 CSS-border arc spinner (`spin 0.75s linear infinite`), border-top `#1d6fdb` | `#1d6fdb` | "Pairing…" |
| `connecting` | `connecting` | same arc spinner as `pairing` | `#1d6fdb` | "Connecting…" |
| `idle` | `idle` | 14×14 SVG: outline circle | `#248a3d` | "Idle" |
| `recording` | `recording` | 14×14 SVG: solid circle, `cam-pulse 1.2s infinite` on the inner `<circle>` | `#c0271f` | "Recording" |

A `.status-dot { display: none }` rule is retained as a no-op safeguard for any historical code path that might still emit the old `<span class="status-dot">` element — current rendering uses `.status-icon` exclusively.

**Status mapping** (from `camera_manager` enums; see [`camera-manager.md`](camera-manager.md) §...):
- `pairing` only applies to BLE cameras (initial add-camera flow before `first_pair_complete`).
- `connecting` applies to both transports. For BLE: any non-READY state once `first_pair_complete` is set. For RC: the slot is associated to the SoftAP but `wifi_status != WIFI_CAM_READY` — either still waiting on the first UDP response after a fresh associate, or demoted from READY by the keepalive silence watchdog (WoL retry loop active). The slot returns to `idle`/`recording` on the next received UDP datagram, or to `disconnected` if the camera disassociates from the AP.
- `idle` and `recording` are gated on `wifi_status == WIFI_CAM_READY` (which BLE drivers also flip at the end of their readiness sequence — the field is overloaded as the universal "fully ready" signal).

### 10.3 Type Badge (RC-emulation cameras)

```
.cam-type-badge
  font-size: 0.6rem, font-weight: 700, UPPERCASE, letter-spacing 0.06em
  padding: 2px 6px, border-radius: 4px
  background: #e8f4fd, color: #2980b9, border: 1px solid #aad4f0
```
Shown on camera cards and paired rows when `cam.type === 'rc_emulation'`. Badge text: "WiFi RC".

### 10.4 Per-Camera Shutter Buttons

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

The Settings modal no longer contains a "Set Date & Time" row — the manual sync button moved to the System Time section header on the home screen (see §8). `openSettings()` no longer fetches `/api/utc`. The modal now contains only the Time Zone select and the Reboot button.

**Reboot button:**
```
display: block, width: calc(100% - 32px), margin: 16px 16px 0
background: #e67e22 (orange), color: #fff
font-size: 0.88rem, font-weight: 700, min-height: 48px, border-radius: 8px
```
- Confirm dialog: "Reboot Controller?\n\nThe device will restart. Paired cameras and settings will be preserved."
- On confirm: `POST /api/reboot`, disable button, show "Rebooting…" with dot animation
- After 3s: `location.reload()`

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
5. Cancel button → `POST /api/pair/cancel`. Server-side this removes the just-registered slot (RC slots are committed at register time; see §9.1 of [`camera-manager.md`](camera-manager.md)).

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
| GET | `/api/logging-state` | — | `{ state: "logging"\|"not_logging"\|"unknown" }` | UI renders `unknown` as the label "Disconnected" (see §7). |
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
| GET | `/api/settings/timezone` | — | `{ tz_offset_hours: int }` | |
| POST | `/api/settings/timezone` | `{ tz_offset_hours: int }` | `{}` | |
| POST | `/api/settings/datetime` | `{ epoch_ms: number }` | `{}` | Only accepted when `session_synced == false` (no live source has won yet — NVS-restored boot value does not block manual entry). Sets system time from browser clock; triggers `open_gopro_ble_sync_time_all()` and `gopro_wifi_rc_sync_time_all()`. The UI calls this from the Sync button in the System Time section header (see §8); it is no longer reachable from the Settings modal. |

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
