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

**Local tick (1 Hz):** The displayed time advances every second between `/api/utc` polls. On each `/api/utc` response, `applyUtcResponse()` stores `clockServerEpochMs = d.epoch_ms` and `clockFetchedAt = performance.now()`. A `setInterval(renderClock, 1000)` then re-renders the date/time lines using `now = clockServerEpochMs + (performance.now() - clockFetchedAt)`. This keeps the seconds digit ticking smoothly without polling `/api/utc` faster than the existing 2 s top-section rate. `renderClock()` is also invoked synchronously at the end of `applyUtcResponse()` so the display resyncs immediately on the freshest server data. When `clockValid == false`, the time line shows `--:--:--` and the date line is empty.

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

### 12.1 Sections (top to bottom)

**Device**
- Time Zone — `<select>` UTC-12 … UTC+14 (whole hours), populated by `buildTimezoneDropdown()` on page load.
  - On change: `POST /api/settings/timezone` with `{ tz_offset_hours: int }`
  - On open: `GET /api/settings/timezone` → selects value
- CAN Baud Rate — `<select>` with 50k/100k/125k/250k/500k/1M options.
  - On change: `POST /api/settings/can-bitrate` with `{ bitrate_bps: N }` and reveal an orange "Reboot to apply new CAN baud rate" hint until the bitrate matches the value at open.
  - On open: `GET /api/settings/can-bitrate` → selects value, hides hint.

**Updates**
- Channel `<select>` (`stable` / `beta`). On change: `POST /api/ota/channel`.
- "Check for updates" `.settings-action-btn` (blue, full-width minus margins).
- `#upd-result` block — populated by `updates.js` when a check completes (info / success / error states).

**Action buttons** (each a blue `.settings-action-btn`, stacked, full-width minus 16 px side margins):
- **Advanced Settings** → opens the Advanced modal (§12.2). Dismisses Settings on open so the two are never stacked; closing Advanced reopens Settings.
- **About** → opens a native browser `alert()` (no modal) with three lines: "Main App: <version>", "Built: <date> <time>", "Recovery App: <version>". Just an OK button. Backed by `GET /api/version`.

**Reboot button** (bottom of modal, orange):
```
display: block, width: calc(100% - 32px), margin: 16px 16px 0
background: #e67e22 (orange-dark), color: #fff
font-size: 0.88rem, font-weight: 700, min-height: 48px, border-radius: 8px
```
- Confirm dialog: "Reboot Controller?\n\nThe device will restart. Paired cameras and settings will be preserved."
- On confirm: `POST /api/reboot`, disable button, show "Rebooting…" with dot animation
- After 3s: `location.reload()`

### 12.2 Advanced Settings Modal

Separate top-sheet modal (`#advanced-overlay`) with the same styling as Settings. Always exactly one of {Settings, Advanced} is visible — never both.

- Opened from the "Advanced Settings" button in Settings; Settings closes simultaneously.
- Closed via the **Back** button (labelled "Back" to reflect that it returns to Settings) or click-outside; Settings reopens.

**Logging section** — collects diagnostic logs for support reports. See [`log-capture.md`](log-capture.md) for the full design.
- Enable Logging toggle (reuses `.toggle-wrap` styling from the home-screen Auto Control toggle).
  - On open: `GET /api/settings/logging-enabled` → applies state.
  - On toggle click: optimistic UI update, `POST /api/settings/logging-enabled` with `{ enabled: bool }`. Reverts on error.
- When ON: a stats line "Ring: X used / Y · N dropped" appears (orange if `dropped > 0`), polled from `GET /api/logs/stats`. Three buttons appear below:
  - **Download log** → `window.location = '/api/logs/download'` (browser saves text file).
  - **Email log** → triggers download, then 400 ms later opens `mailto:imdacrocker@gmail.com` with subject `GoPro Controller log` and a body templated with WHAT-I-WAS-DOING / WHAT-HAPPENED / WHAT-I-EXPECTED placeholders + bold "attach the file you just downloaded" instruction + privacy notice listing what the log contains (MACs, SSIDs) and excludes (passwords, GPS).
  - **Clear log** → confirms with byte count, `POST /api/logs/clear`, refreshes stats.
- When OFF: stats line and three buttons hidden; only the toggle is visible.

**Recovery section**
- **Restart to Recovery** button — orange `.settings-recovery-btn`. Calls `POST /api/ota/reboot-recovery` and reloads after a short delay. Used when a user needs to upload firmware / web UI bundles via the recovery app's embedded form (e.g. when the storage partition is empty).

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

**Sheet panel** (`.manage-modal`):
```
background: #fff
border-radius: 12px 12px 0 0   ← only top corners rounded
width: 100%, max-width: 480px
max-height: 85vh, overflow-y: auto
padding: 0 0 2em
```

**Header:** "Manage Cameras" title left, "Done" button right. Sticky (z-index 10) so it stays visible while scrolling.

**Body layout (top to bottom):**
1. Paired Cameras section (§13.1)
2. Add Camera entry button (§13.2)

On open: calls `refreshModalPairedCameras()` and starts a 3 s interval that refreshes the paired list. No other polling fires from this modal — the RC-discovered list and the BLE scan both live in the Add Camera modal (§13.3) now.

On close: clears the 3 s paired-list interval. Modal state otherwise persists.

Clicking the backdrop also closes the modal.

### 13.1 Paired Cameras

Title: "PAIRED CAMERAS" + count badge (hidden when 0). Rendered in `--text-primary` (dark) via an inline `style="color:var(--text-primary)"` override on the `.modal-section-title` span, to mark it as a list heading.

Source: `GET /api/paired-cameras`

Each camera renders as a `.modal-paired-row` (background `#fafafa`, border `#eee`, border-radius 8px):
- Left: name line (`0.9rem`, weight 600) with optional type badge (shown when `cam.type === 'rc_emulation'`, text "WiFi RC"); meta line (`0.72rem`, `#999`) showing name · model_name · IP/addr depending on what the API returned.
- Right: "Remove" button in red `#e74c3c`.

**Remove flow:** Confirm dialog → `POST /api/remove-camera` with `{ slot }` → `refreshModalPairedCameras()`. No follow-up RC-discovered refresh here — that list isn't in this modal anymore. Hero3/Hero4 slot reuse is picked up the next time the user enters the Add Camera → Hero3/Hero4 instructions pane (§13.3.2).

### 13.2 Add Camera entry button

Single full-width primary button rendered as the **last** element in the Manage modal body (after the Paired Cameras list, not before — so removal of an existing camera and adding a new one share the same visual flow).

```
id: add-camera-btn
.modal-action-btn pattern: block, width: calc(100% - 32px), 16px side margin
background: var(--blue), color: var(--white)
font-size: 0.88rem, font-weight: 700, min-height: 44px, border-radius: 8px
```

On click:
1. `closeModal()` — dismisses the Manage Cameras modal.
2. `openAddCameraModal()` — opens the Add Camera modal (§13.3) in its initial "list" state.

There is no info-icon / tooltip on this button (the previous two buttons each had one; the new flow surfaces all needed guidance on the per-model instructions pane).

### 13.3 Add Camera Modal (Bottom Sheet, two-pane slider)

A separate bottom-sheet modal opened from the Manage modal's Add Camera button. The user picks a camera model, then slides to a model-specific instructions screen, then either taps Pair (BLE flow) or Add (RC flow) — both of which hand off to the existing pair-progress overlay.

```
id: add-camera-overlay
.modal-overlay shared rules: position: fixed, inset: 0, background: rgba(0,0,0,0.45), z-index: 200
align-items: flex-end, justify-content: center   ← shared with #modal-overlay via combined selector
```

**Sheet panel** (`.manage-modal .add-camera-modal`):
```
background: #fff
border-radius: 12px 12px 0 0
width: 100%, max-width: 480px
max-height: 85vh
display: flex, flex-direction: column   ← so the slider track can fill the body
overflow: hidden                         ← clips the 200%-wide slider track
padding-bottom: 0
```

**Header** (left → right):
- `#add-camera-back` — `.modal-back-btn` (blue text + chevron SVG, "Back" label, 0.92 rem weight 600). Hidden on the list pane via the `hidden` attribute; revealed by `selectCameraModel()` and re-hidden by `slideToList()`. Tap → slides back to the model picker.
- `.modal-title` — "Add New", centered visually because the back button is hidden on the initial pane.
- `#add-camera-cancel` — `.modal-done-btn` (blue "Cancel" text). Always closes the whole modal. Closing from the instructions pane resets the modal back to the list pane so the next open starts fresh.

Backdrop click is equivalent to Cancel.

**Slider mechanics:**
```
#add-camera-content   flex: 1 1 auto, min-height: 320px, overflow: hidden, position: relative
  #add-camera-track   display: flex, width: 200%, height: 100%
                      transition: transform 0.28s ease
                      transform: translateX(0)        ← list pane visible (default)
                      transform: translateX(-50%)     ← when .step-instructions is added
    .add-camera-pane  width: 50% (of track = 100% of content), flex-shrink: 0
                      overflow-y: auto, -webkit-overflow-scrolling: touch
                      padding: 8px 0 24px
```

Both panes are in the DOM at all times. Vertical scroll is per-pane (mobile inertia preserved). The `.step-instructions` class on `#add-camera-track` toggles the slide.

#### 13.3.1 Model picker pane

`#add-camera-list-pane` contains an `<ul class="camera-model-list">` of 8 rows, one per supported model, in this order:

```
Hero3
Hero4
Hero7
Hero9
Hero10
Hero11
Hero12
Hero13
```

Each row:
```
.camera-model-row
  padding: 16px 20px
  min-height: 56px   ← finger-friendly tap target on mobile
  font-size: 1rem, font-weight: 600, color: --text-primary
  border-bottom: 1px solid --gray-border-light   ← collapses on :last-child
  display: flex, align-items: center, justify-content: space-between
  ::after content: "›"   ← chevron, 1.6rem, --gray-light
  :hover, :active { background: var(--gray-bg) }
  user-select: none, cursor: pointer
```

The list is scrollable inside the pane when the viewport is shorter than the total list height. On a typical phone all 8 rows fit without scrolling.

Each row carries a `data-model="HeroN"` attribute. A single `forEach(row => row.addEventListener('click', …))` wires every row to `selectCameraModel(row.dataset.model)`.

#### 13.3.2 Instructions pane

`#add-camera-instructions-pane` contains, top to bottom:

1. `<p id="add-camera-instructions" class="add-camera-instructions">` — the per-model body text, written by `selectCameraModel()` via `innerHTML` (so `<br>` line breaks survive). 0.95 rem, 1.45 line-height, `--text-primary`, 4 px / 20 px / 16 px / 20 px padding.
2. `#add-camera-ble-section` — visible for BLE models (Hero7 / Hero9–Hero13), hidden for RC models. Contains:
   - `#add-camera-scan-btn` — `.modal-action-btn`. Idle: blue `#2980b9`, label "Scan". Scanning: background `none`, `1 px --gray-border` outline, color `#333`, label "Cancel Scan". Single click handler that toggles `startScan()` / `cancelScan()`.
   - `#add-camera-status` — small grey status line (`0.85 rem`, `--gray-label`, 20 px side padding, `min-height: 1.4em`). Carries countdown ("Scanning… Ns"), end-of-scan messages ("Scan complete." / "Scan cancelled."), and the RC "Cannot add — IP address not yet assigned. Wait a moment and try again." message.
   - `#add-camera-ble-results` — populated by `renderFoundCameras()` as `.found-camera-row` entries with name + address + RSSI and a blue Pair button.
3. `#add-camera-rc-section` — visible for RC models (Hero3 / Hero4), hidden for BLE models. Contains `#add-camera-rc-results`, populated by `renderRcDiscovered()` with `.found-camera-row` entries (name "Unknown Device", `addr · ip` meta, blue Add button) and a header line that reads "No unidentified devices connected." or "{N} device(s) connected — click Add to probe:".

**Model → flow mapping** (`RC_MODELS = new Set(['Hero3', 'Hero4'])`):

| Model | Flow | Instruction body |
|---|---|---|
| Hero3, Hero4 | WiFi RC discovered list | "Reset all wireless connections on the camera, and then choose to pair with a new WiFi RC." |
| Hero7 | BLE Scan | "Reset all wireless connections on the camera. After resetting you MUST update the camera Wifi to 2.4ghz manually. Then, choose to pair with the GoPro App. Once in Pairing mode, click Scan.&lt;br&gt;&lt;br&gt;NOTE: The wireless connection to this browser may disconnect during this process. If this happens, reconnect to the controller's WiFi and refresh." |
| Hero9, Hero10, Hero11, Hero12, Hero13 | BLE Scan | "Reset all wireless connections on the camera, and then choose to pair with the GoPro App. Once in Pairing mode, click Scan." |

The Hero7 message exists because the controller's STA-side join during the pair-complete handshake (see [`hero7_pair_complete`](../../apps/main/components/gopro/open_gopro_ble/pair_complete.c) and the §12 / CHANGELOG entry for 5 GHz fail-fast) briefly drops the SoftAP, which may disconnect the user's browser if they're connected to the controller's own Wi-Fi. The "MUST switch to 2.4 GHz" reminder warns the user before they try, instead of relying on the 5 GHz fail-fast error to teach them after the fact.

**`selectCameraModel(model)`** body:
1. Reveal `#add-camera-back`.
2. Write the instructions HTML via `instructionsForModel(model)`.
3. Branch on `RC_MODELS.has(model)`:
   - RC branch: show `#add-camera-rc-section`, hide BLE section, set `rcListActivated = true`, call `refreshRcDiscovered()` immediately, then `setInterval(refreshRcDiscovered, 3000)` on `addCameraRcPollTimer`.
   - BLE branch: show `#add-camera-ble-section`, hide RC section, clear `#add-camera-ble-results`, clear status, reset the Scan button to "Scan" idle styling.
4. Add `.step-instructions` to `#add-camera-track` → CSS transition slides the pane in.

**`slideToList()`** body (Back button / part of close): cancel scan if active, clear `addCameraRcPollTimer`, `rcListActivated = false`, remove `.step-instructions`, hide back button, clear both result containers, clear status.

**`closeAddCameraModal()`**: same as `slideToList()` plus `addCameraOverlay.classList.remove('open')` and `resetAddCameraModal()` (which also clears the instructions text).

#### 13.3.3 BLE scan flow

When the active model is BLE-flow and the user taps the Scan button:
1. `scanning = true`, snapshot already-paired addresses for filtering.
2. Pause `cameraStatusTimer`, `topSectionTimer`, and `modalPairedRefreshTimer` (the last is already null since Manage is dismissed, but the call is idempotent). This keeps the ESP32 from juggling 4 concurrent HTTP requests while NimBLE is active.
3. `POST /api/scan`.
4. Start 1 s `pollScanResults()` interval — `GET /api/cameras`, filtered against the snapshot.
5. Start 1 s countdown updating `#add-camera-status` from "Scanning… 120s" down to 0; auto-stop at 0 via `stopScan(false)`.
6. Cancel button or tapping Pair on a row → `cancelScan()` → `POST /api/scan-cancel` → `stopScan(true)` → resume `cameraStatusTimer` + `topSectionTimer`.

`renderFoundCameras()` writes into `#add-camera-ble-results`. Each row carries `data-addr` and `data-addr-type`; the delegated click handler hands off to the pair-progress modal (§13.3.5) with `POST /api/pair { addr, addr_type }`.

#### 13.3.4 WiFi RC add flow

When the active model is RC-flow:

`GET /api/rc/discovered` polls every 3 s while the instructions pane is showing a Hero3/Hero4. Returns `[{ addr, ip }]` for SoftAP-connected stations whose MAC OUI is on the GoPro allow-list (see `GOPRO_RC_OUIS[]` in `api_rc.c`) and that aren't already in a managed RC slot. Non-GoPro stations are filtered server-side so the list stays small.

The poll is gated by `rcListActivated` (so callers that don't care — e.g. a stray late-arriving response — short-circuit). The flag is set in the RC branch of `selectCameraModel()` and cleared by `slideToList()` / `closeAddCameraModal()`.

`renderRcDiscovered()` writes into `#add-camera-rc-results`. Empty list shows "No unidentified devices connected." (`.modal-empty` style). Non-empty shows the count header plus one `.found-camera-row` per device (name "Unknown Device", `addr · ip` or "IP pending" meta, Add button).

The delegated click handler:
1. If `ip` is missing, write the "Cannot add — IP address not yet assigned. Wait a moment and try again." message to `#add-camera-status` and abort.
2. Otherwise clear the results, open the pair-progress modal (§13.3.5), and `POST /api/rc/add { addr, ip }`.

#### 13.3.5 Pair-progress modal handoff

Both Pair (BLE) and Add (RC) call `openPairModal()` first, which:
1. Closes the Manage Cameras modal if open (defensive — at this point it should already be closed).
2. Closes the Add Camera modal if open (the common case — this is the modal that just launched the pair).
3. Reveals `#pair-overlay` with throbber + "Connecting to camera…" + Cancel.

The pair-progress overlay then polls `GET /api/pair/status` every 1 s for `success` / `failed`. Both transports drive the same overlay; only the API endpoint that kicked off the attempt differs.

- `success` → throbber → green ✓ → "Success!" → auto-dismiss after 2 s → `refreshCameraStatus()` repaints the home-screen camera list.
- `failed` → throbber → red ✕ → mapped error label (`PAIR_ERROR_LABEL` in `app.js`, e.g. `handshake_timeout` → "Camera setup timed out. Try again."); button changes to "OK".
- Cancel (mid-pair) → `POST /api/pair/cancel` → server returns FAILED with `error_code: "cancelled"` → button becomes "OK".

On terminal dismiss the home screen takes focus — the user is not returned to either the Manage or Add Camera modals.

---

## 14. Polling Summary

| Timer | Interval | Endpoints | Purpose |
|---|---|---|---|
| Camera status | 3s | `GET /api/paired-cameras` | Camera cards on main screen |
| RC status + UTC + auto-control | 2s | `GET /api/logging-state`, `GET /api/utc`, `GET /api/auto-control` | Top two sections |
| Clock display tick | 1s | — (local only) | Smooth seconds tick in System Time; see §8 |
| BLE scan results | 1s (during scan only) | `GET /api/cameras` | Found cameras list in the Add Camera modal (§13.3.3) |
| Manage modal refresh | 3s (Manage modal open only) | `GET /api/paired-cameras` | Paired list in Manage Cameras modal (§13.1) |
| Add Camera RC discovered | 3s (Hero3/Hero4 instructions pane only) | `GET /api/rc/discovered` | SoftAP-connected devices list in the Add Camera modal (§13.3.4) |
| Reconnect probe | 1s (disconnected only) | `GET /api/version` (raw `fetch`) | Detects controller recovery; reloads page on first 2xx. See §14a |

All polls fire independently via `setInterval`; no coordination or debouncing between timers. The clock display tick consumes no network.

### 14a. Disconnect detection

Every call through `apiFetch()` contributes to a connection-health counter:

- Success → `consecutiveFailures = 0`.
- Failure (network error *or* non-2xx) → `consecutiveFailures++`. When the counter reaches `DISCONNECT_THRESHOLD = 3` and we are not already disconnected, `enterDisconnected()` fires.

With the polls above feeding the counter, idle detection lands in ~3–6 s of true unreachability (faster during a BLE scan or pair attempt, where 1 s polls are also feeding in). 5xx responses count too — the UI treats "server is broken" the same as "server is gone."

`enterDisconnected()`:

1. Clears every UI timer: top-section, camera status, modal refresh, BLE scan poll, scan countdown, pair-status poll, pair-dismiss timeout, and the clock-tick timer. No further requests fire from the page.
2. Hides every visible modal — removes `.open` from all `.modal-overlay` and sets the `#pair-overlay` `hidden` attribute. No teardown calls (we're about to reload on recovery anyway).
3. Reveals `#disconnect-overlay` (§14b).
4. Starts `reconnectProbeTimer = setInterval(reconnectProbe, 1000)`.

`reconnectProbe()` uses raw `fetch('/api/version')` with a 1.5 s `AbortController` timeout — it deliberately bypasses `apiFetch` so it does not feed the failure counter (which would loop) and does not reset `consecutiveFailures` on success. On the first 2xx response it sets `reloading = true`, clears the probe interval, and calls `location.reload()`.

### 14b. Disconnect overlay

```
id: disconnect-overlay
position: fixed, inset: 0
z-index: 9999                ← above the pair-progress overlay (1000) and all modals (200)
background: var(--white)
display: flex                ← centered both axes
hidden by default via the `hidden` attribute
```

Inner `.disconnect-box`: `max-width: 320px` (fits the smallest common phone, ~320 CSS px), centered column. Contents top-to-bottom:

- `.disconnect-icon` — inline SVG no-Wi-Fi symbol (three concentric arcs + base dot, diagonal slash), 96×96 px, `color: #000`.
- `.disconnect-title` — text `DISCONNECTED`, font-size `2.2rem`, weight `800`, black. Sized to fill most of the 320 px box at 12 characters.
- `#disconnect-reload-btn` — clones the `#manage-btn` style: `background: var(--blue)`, white text, `min-height: 48px`, `border-radius: 8px`, full-width inside the 320 px box. Click → `reloading = true; location.reload()`.

The overlay is fully opaque — no UI bleeds through. Tapping the backdrop does nothing (no dismiss handler).

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
| `modalPairedRefreshTimer` | interval ID | `null` | 3 s Manage modal paired-list refresh (§13.1) |
| `addCameraRcPollTimer` | interval ID | `null` | 3 s `GET /api/rc/discovered` poll while on the Hero3/Hero4 instructions pane (§13.3.4) |
| `rcListActivated` | bool | `false` | Gates `refreshRcDiscovered()` so stray late-arriving polls short-circuit. Set in the RC branch of `selectCameraModel()`, cleared by `slideToList()` / `closeAddCameraModal()` (§13.3.2) |
| `clockTickTimer` | interval ID | `null` | 1s local clock display tick (§8) |
| `clockServerEpochMs` | int | `0` | Last `epoch_ms` from `/api/utc` |
| `clockFetchedAt` | DOMHighResTimeStamp | `0` | `performance.now()` when `clockServerEpochMs` was stored |
| `clockValid` | bool | `false` | Latest `/api/utc` `valid` flag; gates display vs. `--:--:--` |
| `clockSessionSynced` | bool | `false` | Latest `/api/utc` `session_synced` flag |
| `consecutiveFailures` | int | `0` | Connection-health counter, fed by every `apiFetch` |
| `disconnected` | bool | `false` | Set by `enterDisconnected`; guards re-entry |
| `reloading` | bool | `false` | Set just before `location.reload()` so late-arriving success paths don't double-reload |
| `reconnectProbeTimer` | interval ID | `null` | 1s `/api/version` probe while disconnected |

Constants:
- `DISCONNECT_THRESHOLD = 3` (consecutive `apiFetch` failures before the disconnect overlay appears).
- `RC_MODELS = new Set(['Hero3', 'Hero4'])` (camera models that take the WiFi-RC discovered-devices flow in the Add Camera modal; everything else takes the BLE Scan flow — see §13.3.2).

---

## 17. Open Items / Known V2 Decisions

- **Camera type field:** V2 API uses `"rc_emulation"` for Hero 4 and `"ble"` for Hero 9+ (not `"legacy_wifi"` / `"cohn"`). UI badge text is "WiFi RC" / "BLE". ✅ Resolved.
- **Model selection for RC-emulation cameras:** Firmware defaults to `HERO4_BLACK`; no model picker in the UI. ✅ Resolved (deferred).
- **BLE-control cameras (Hero 9+):** Pairing alone is sufficient — there is no separate provisioning step. The post-readiness sequence (`GetHardwareInfo` → `SetCameraControlStatus(EXTERNAL)` → `SetDateTime` → status poll) runs automatically inside `open_gopro_ble`. ✅ Resolved.
- **Status granularity:** Five states for BLE cameras (`disconnected` / `pairing` / `connecting` / `idle` / `recording`); four for WiFi (`disconnected` / `connecting` / `idle` / `recording`). The `pairing` vs `connecting` distinction (BLE only) is driven by the persisted `first_pair_complete` flag. RC `connecting` covers any time the slot is associated to the SoftAP but `wifi_status != WIFI_CAM_READY` — including the keepalive-silence WoL-retry window.
- **Pair-attempt polling:** Both the BLE Add flow and the WiFi RC Add flow drive the same `pair-overlay` modal and poll `GET /api/pair/status` every ~1 s until the state reaches `success` or `failed`. Errors are mapped to user-friendly messages by `PAIR_ERROR_LABEL` in `app.js`. The `model_unsupported` code surfaces frozen models (predicate is currently empty — reserved for future use). The `pair_complete_failed` code surfaces a specific message from the orchestration — typically `"Camera on 5 GHz; set Wi-Fi Band to 2.4 GHz, re-pair"` for Hero6/7/8 that haven't been switched to 2.4 GHz before pairing. RC failures show as `handshake_timeout` (no UDP response in 20 s) or `slots_full`. The 15-second arbitrary refresh delay used in the pre-shared-modal RC flow is gone.
- **Color palette:** V2 should use CSS custom properties (`:root { --blue: #2980b9; … }`) for maintainability.
- **Settings → Device section:** May need additional entries (e.g. per-camera name edit) — TBD.
- **Timezone half-hours:** Whole-hour offsets only. Not a V2 priority (e.g. UTC+5:30 would need `float` or half-hour `int`).
