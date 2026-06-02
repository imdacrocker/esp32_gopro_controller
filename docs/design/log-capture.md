# Diagnostic Log Capture Design

**Status:** Proposed
**Date:** 2026-05-16

Cross-session reference for the in-device log ring buffer and the user-facing
report flow. Companion to [`camera-manager.md`](camera-manager.md),
[`ota.md`](ota.md), and [`web-ui.md`](web-ui.md).

---

## 1. Goals

- Give testers and users a frictionless way to send diagnostic logs back to the
  maintainer when something misbehaves.
- Capture **all** `ESP_LOG` output from boot onward without losing early
  messages, while keeping the UART console at its normal INFO verbosity for
  developers.
- Operate entirely within the SoftAP world — no outbound network connection
  from the device, no SMTP, no cloud middleware.
- Cost a known, fixed amount of RAM and flash; never grow unbounded.

### Non-goals (v1)

- Crash / panic log persistence across reboots. Out of scope; the existing
  `esp_core_dump` mechanism can be added later as an independent feature.
- Structured event logging alongside `ESP_LOG`. Out of scope.
- Runtime filtering, search, or live tail in the web UI.
- Authentication on the log endpoints. The device is on its own SoftAP and the
  surface is already trust-on-attach.

---

## 2. Summary & locked decisions

| Topic | Decision |
|---|---|
| Capture scope | All `ESP_LOG*` output, hooked via `esp_log_set_vprintf` |
| Storage | **64 KB static RAM ring** in internal SRAM (`.bss`), oldest-line eviction. (Started at 64 KB; bumped to 128 KB after first field test, then reverted: 128 KB shrank the largest contiguous heap block to ~7.7 KB and broke `read_file()` for every web UI asset. To enlarge further, move the ring to PSRAM rather than growing `.bss`.) |
| Noisy-tag silencing | A short allow-list of known-noisy ESP-IDF internal tags (`httpd_*`, `vfs_calls`, `efuse`, `intr_alloc`, `temperature_sensor_hal`, `nvs`) is held at `ESP_LOG_INFO` even though the global runtime level is `DEBUG`. Cuts ring volume by ~85% — the first field test was 99.9% IDF internals with zero useful content. |
| Persistence | None. Reboot clears the ring. |
| Console behaviour | UART continues to show INFO+ only; ring receives DEBUG+ |
| Compile-time max level | `CONFIG_LOG_MAXIMUM_LEVEL_DEBUG=y` (VERBOSE excluded) |
| Default runtime level | `CONFIG_LOG_DEFAULT_LEVEL_INFO=y` (clean boot before hook installs) |
| Runtime level after hook install | `esp_log_level_set("*", ESP_LOG_DEBUG)` |
| Component layout | New `apps/wireless/components/log_ring/`. HTTP endpoints live in `http_server/api_logs.c`. |
| Endpoints | `GET /api/logs/download`, `POST /api/logs/clear`, `GET /api/logs/stats` |
| Web UI | New **Diagnostics** section: three buttons (Download log, Email log, Clear log) plus a "lines dropped: N" indicator |
| Email recipient | Hard-coded `imdacrocker@gmail.com` for v1 (Kconfig'd for later override) |
| Email body | Templated mailto with privacy warning + bold attach-the-file instruction |
| Compression on download | Skipped for v1. Add later if attachment size becomes a problem. |
| User-facing kill switch | "Enable Logging" toggle in the **Advanced Settings** modal (one step deeper than the gear menu — gear → Advanced Settings). Default **OFF**. Persisted in NVS. When OFF: hook stops writing to ring, ring is cleared, and the Download/Email/Clear buttons are hidden. |
| Setting location | The toggle and three action buttons live inside the Advanced Settings modal, alongside Restart-to-Recovery. (Originally placed directly in Settings; relocated 2026-05-17 when Advanced Settings became its own modal.) |
| Authentication | None |

---

## 3. Architecture overview

```
  any task                        ┌──────────────────┐
  calls ESP_LOGx ───► esp_log ───►│ log_tee vprintf  │──► stdout (UART)
                                  │  hook            │     [I/W/E only, always]
                                  │                  │
                                  │  if enabled:     │──► log_ring  (RAM, 64 KB)
                                  │                  │     [all levels]
                                  └──────────────────┘
                                           ▲
                                           │ s_enabled (atomic bool)
                                           │
                                  ┌────────┴──────────────────────┐
                                  │                               │
   browser ── POST /api/settings/logging-enabled ──► api_settings ─┤
                                                                   │
   browser ── GET /api/logs/download ──► api_logs.c ──► log_ring_stream()
   browser ── POST /api/logs/clear  ──► api_logs.c ──► log_ring_clear()
```

The hook is **always installed at boot** regardless of the toggle state — this
guarantees the UART console keeps working and that flipping the toggle ON
mid-session immediately starts capturing without a reboot. The toggle only
gates the ring write; UART echo is unconditional.

The `log_ring` component owns the buffer, the `vprintf` hook, the mutex, and
the snapshot/clear API. `http_server` is the only other component that depends
on it (via `api_logs.c`).

---

## 4. New component: `log_ring`

Lives at `apps/wireless/components/log_ring/`. Recovery does **not** depend on it
— recovery stays minimal per `ota.md`.

### Public API (`include/log_ring.h`)

```c
/* Install the esp_log vprintf hook and raise the runtime level to DEBUG.
 * Does NOT load the NVS-persisted enable state — call log_ring_set_enabled()
 * after NVS is up. The ring starts enabled by default so any logs emitted
 * before NVS init are still captured.
 * Idempotent. Safe to call as the very first line of app_main(). */
esp_err_t log_ring_init(void);

/* Enable/disable ring capture at runtime. When transitioning ON→OFF, the
 * ring is cleared as part of the same call. Does NOT touch NVS — call
 * log_ring_save_enabled_to_nvs() separately to persist. UART echo is
 * unaffected; this only gates writes into the ring. */
void log_ring_set_enabled(bool enabled);

bool log_ring_is_enabled(void);

/* Persist the current enable state to NVS (namespace "logring", key "enabled"). */
esp_err_t log_ring_save_enabled_to_nvs(void);

/* Read the persisted enable state from NVS and apply it via
 * log_ring_set_enabled(). Missing key → default OFF.
 * Call once at boot, immediately after nvs_flash_init(). */
void log_ring_load_persisted_enabled(void);

/* Stream the ring contents to a callback in ~2 KB chunks. The internal
 * mutex is taken only per-chunk, so log writes can interleave; any bytes
 * evicted mid-stream are skipped. No heap allocation — chosen over the
 * earlier "malloc the whole snapshot" plan after a 64 KB malloc failed
 * against a fragmented heap (200 KB free total but no contiguous block). */
typedef esp_err_t (*log_ring_write_cb_t)(void *arg, const char *data, size_t n);
esp_err_t log_ring_stream(log_ring_write_cb_t cb, void *arg);

/* Reset head/tail; counters preserved except 'used' which goes to 0. */
void log_ring_clear(void);

typedef struct {
    size_t   capacity;             /* always RING_SIZE */
    size_t   used;                 /* bytes currently held */
    uint64_t bytes_written_total;  /* lifetime, never reset */
    uint64_t lines_dropped_total;  /* lifetime, never reset */
    uint32_t oldest_uptime_ms;     /* esp_log_timestamp() of oldest line */
} log_ring_stats_t;

void log_ring_get_stats(log_ring_stats_t *out);
```

### Internal layout

```c
#define LOG_RING_SIZE (64 * 1024)

static char           s_ring[LOG_RING_SIZE];  /* .bss */
static size_t         s_head;                 /* next write offset */
static size_t         s_tail;                 /* oldest byte offset */
static size_t         s_used;
static uint64_t       s_total_written;
static uint64_t       s_total_dropped;
static uint32_t       s_oldest_ts_ms;
static SemaphoreHandle_t s_mtx;               /* recursive: no — hook never re-enters */
static vprintf_like_t s_chain;                /* previous vprintf, called for UART echo */
static atomic_bool    s_enabled = true;       /* toggle; starts ON for boot capture */
```

### Eviction rule

Whole-line eviction. When a new line of length `n` doesn't fit:

1. Lock the mutex.
2. While `s_used + n > LOG_RING_SIZE`: advance `s_tail` to the byte after the
   next `'\n'`, decrement `s_used` by the bytes skipped, increment
   `s_total_dropped`.
3. memcpy the new line in at `s_head`, advance `s_head` (modulo `LOG_RING_SIZE`),
   `s_used += n`, `s_total_written += n`. Update `s_oldest_ts_ms` if the ring
   was empty before.
4. Release the mutex.

The ring is treated as a circular buffer of bytes, but evictions always land on
line boundaries so the downloaded file never starts mid-message.

### The `vprintf` hook

```c
static int log_tee(const char *fmt, va_list args)
{
    char buf[256];                     /* one line; truncates anything longer */
    va_list copy;
    va_copy(copy, args);
    int n = vsnprintf(buf, sizeof(buf), fmt, copy);
    va_end(copy);

    if (n <= 0) return n;
    if ((size_t)n >= sizeof(buf)) n = sizeof(buf) - 1;

    if (atomic_load(&s_enabled)) {
        ring_write(buf, (size_t)n);
    }

    /* Echo to UART for INFO and above only. ESP-IDF formats each line as
     * "<LEVEL_CHAR> (<ts>) <tag>: <msg>\n", so buf[0] is the level letter.
     * UART echo is unconditional — independent of the ring's enable flag. */
    if (buf[0] != 'D' && buf[0] != 'V') {
        fwrite(buf, 1, (size_t)n, stdout);
        fflush(stdout);
    }
    return n;
}
```

Notes:

- The 256-byte line cap mirrors ESP-IDF's own internal buffer in
  `esp_log_writev`. Lines longer than that are truncated — fine for our
  purposes.
- `va_copy` is required because `vsnprintf` consumes the `va_list`. We don't
  need the second copy because the hook doesn't re-invoke the original
  vprintf with `args` — it writes the already-formatted bytes via `fwrite`,
  which is faster and sidesteps reformatting.
- The mutex is not taken in interrupt context. `ESP_EARLY_LOGx` and
  `ESP_DRAM_LOGx` calls bypass `vprintf` entirely and go straight to the UART,
  so the hook never runs from an ISR.
- If `xSemaphoreTake` fails (shouldn't, but defensively), the line is dropped
  and `s_total_dropped` is incremented.

### Kconfig

`apps/wireless/components/log_ring/Kconfig`:

```
menu "Log Ring"

config LOG_RING_SIZE_KB
    int "Ring buffer size (KB)"
    default 64
    range 8 256
    help
        Size of the in-RAM log ring buffer. Consumes static (.bss) memory.

config LOG_RING_DEFAULT_LEVEL
    int "Runtime log level after init (raised by hook)"
    default 4
    range 1 5
    help
        Numeric ESP log level fed to esp_log_level_set("*", ...) after the
        vprintf hook is installed. 4 = DEBUG, 5 = VERBOSE.

endmenu
```

---

## 4a. Enable-toggle persistence

User-visible toggle in the Settings modal, persisted across reboots via NVS.

| Property | Value |
|---|---|
| NVS namespace | `logring` (owned by the `log_ring` component) |
| NVS key | `enabled` |
| Type | `u8` (0 = disabled, 1 = enabled) |
| Default if key missing | `0` (disabled) |
| Behaviour on ON → OFF | `log_ring_clear()` runs as part of `log_ring_set_enabled(false)`, atomically with the flag flip, so by the time a reader sees the flag as false the ring is also empty. |
| Behaviour on OFF → ON | Flag flips. Ring is already empty (cleared on the previous OFF transition, or empty from boot). Capture resumes immediately. |
| Boot behaviour | Hook installs with `s_enabled=true` so very-early boot logs are captured. After NVS init, `app_main` calls `log_ring_load_persisted_enabled()` which reads the NVS key and applies the toggle; if the persisted value is OFF (the default), the boot logs are cleared at that moment. Acceptable tradeoff — early boot logs are mostly noise, and the alternative (defer the hook until after NVS) drops messages from `nvs_flash_init()` itself, which is exactly the kind of failure a user might be trying to report. |

NVS persistence is owned by `log_ring` itself, matching the codebase pattern
where each component manages its own persistent settings (e.g. `can_manager`
owns timezone and CAN-bitrate NVS). `api_settings.c` just calls
`log_ring_set_enabled()` + `log_ring_save_enabled_to_nvs()` on POST and
`log_ring_is_enabled()` on GET.

---

## 5. sdkconfig changes

Add to `apps/wireless/sdkconfig.defaults`:

```
# Diagnostic log ring
CONFIG_LOG_MAXIMUM_LEVEL_DEBUG=y
CONFIG_LOG_DEFAULT_LEVEL_INFO=y
```

Result: DEBUG strings are compiled into the binary (+1–3 KB flash), but the
boot path stays quiet on UART until `log_ring_init()` runs.

---

## 6. Boot integration

`apps/wireless/main/main.c` — `log_ring_init()` becomes the **first** call in
`app_main`, before NVS init. This guarantees we capture errors from every
subsequent subsystem.

```c
void app_main(void)
{
    log_ring_init();                /* installs hook + raises level to DEBUG; ring starts enabled */
    ESP_ERROR_CHECK(nvs_flash_init());

    /* Apply the persisted logging toggle. Done here, immediately after NVS,
     * so subsequent subsystem init logs honour the user's setting. If the
     * persisted value is OFF (the default for a fresh device), the small
     * handful of pre-NVS log lines already captured will be cleared by this
     * call — acceptable. */
    log_ring_load_persisted_enabled();

    /* ... existing boot order continues ... */
}
```

This precedes the existing boot order recorded in
[`memory/project_overview.md`](../../memory/project_overview.md) — that
sequence stays unchanged; `log_ring_init` is simply prepended.

---

## 7. HTTP endpoints

New file: `apps/wireless/components/http_server/api_logs.c`. Registered from
`driver.c` like the other `api_*_register()` calls. The component's
`CMakeLists.txt` gains `log_ring` in `REQUIRES`. The `max_uri_handlers` ceiling
in `driver.c` grows by 3.

### `GET /api/logs/download`

- `Content-Type: text/plain; charset=utf-8`
- `Content-Disposition: attachment; filename="gopro-ctrl-<mac6>-<stamp>.txt"`
  - `<mac6>` = last 6 hex chars of the SoftAP MAC (no separator)
  - `<stamp>` = UTC `YYYYMMDDTHHMMSSZ` if `utc_valid`, else `up<seconds>s` from
    `esp_timer_get_time()`
- Body: a header block followed by a blank line followed by the ring snapshot.

Example header block (prepended server-side):

```
# device:    AA:BB:CC:DD:EE:FF
# firmware:  1.4.2 (May 16 2026 14:01:33) channel:beta
# captured:  2026-05-16T14:32:11Z  (utc_valid:true session_synced:true)
# uptime:    1843 s
# ring:      65536 bytes capacity, 11423 used, 0 lines dropped (lifetime)
#
```

Implementation: take the snapshot under the mutex, release, build the header
into a small stack buffer, then `httpd_resp_send` two chunks (header, body).
Free the snapshot before returning.

### `POST /api/logs/clear`

- Returns `{ "cleared_bytes": <n> }` (number of bytes that were in the ring at
  call time). Does not reset lifetime counters.

### `GET /api/logs/stats`

- Returns `log_ring_stats_t` as JSON:
  ```json
  {
    "capacity": 65536,
    "used": 11423,
    "bytes_written_total": 482109,
    "lines_dropped_total": 0,
    "oldest_uptime_ms": 8421
  }
  ```

### `GET /api/settings/logging-enabled`

- Lives in `api_settings.c` alongside the other settings endpoints.
- Returns `{ "enabled": true|false }` from NVS (defaults to `true` if unset).

### `POST /api/settings/logging-enabled`

- Body: `{ "enabled": true|false }`.
- Writes NVS, calls `log_ring_set_enabled()`, returns the new state.
- When transitioning to `false`, the ring is cleared inside
  `log_ring_set_enabled()` before the function returns — by the time the HTTP
  response goes out, the data is already gone.

### Behaviour when logging is disabled

- `GET /api/logs/download`, `POST /api/logs/clear`, `GET /api/logs/stats` all
  still respond normally. Download returns just the header block followed by
  an empty body. The UI hides the buttons in the disabled state, but the
  endpoints are not gated server-side — if someone hits them directly nothing
  bad happens, they just get an empty ring.

### Out of v1 scope

- `GET /api/logs` (inline view) — UI can fetch the download URL via XHR if it
  wants to render in-page; deferred until a real need.
- `?since=<offset>` incremental tail — deferred.
- gzip on the download — text compresses ~5×; revisit when an attachment
  exceeds typical mail limits.

---

## 8. Web UI

All log-related controls live inside the **Advanced Settings** modal —
reachable via gear icon → Advanced Settings button. There is no standalone
Diagnostics section on the home page. Follows the visual conventions
documented in [`web-ui.md`](web-ui.md) §12.3.

### Advanced Settings modal — Logging section

```
┌─ Advanced ───────────────────────────────────────────────┐
│  ── Logging ──────────────────────────────────────────── │
│                                                          │
│  Enable Logging                          [ ●━━○ ON ]     │
│                                                          │
│  (when ON, the rest of the section is visible:)          │
│                                                          │
│  Ring: 11.2 KB used / 64 KB · 0 dropped                  │
│                                                          │
│  [ Download log ]                                        │
│  [ Email log ]                                           │
│  [ Clear log ]                                           │
│                                                          │
│  ── Recovery ─────────────────────────────────────────── │
│                                                          │
│  [ Restart to Recovery ]   (orange)                      │
└──────────────────────────────────────────────────────────┘
```

Opening Advanced Settings dismisses the Settings modal (the two never
stack). Closing Advanced returns to Settings.

When the toggle is **OFF**, the stats line and all three buttons are hidden
(via the existing `[hidden]` convention from `web-ui.md` §2). The toggle
itself stays visible so the user can turn it back on.

### Toggle interaction

- Advanced opens → fetch `GET /api/settings/logging-enabled`, set toggle state.
- User flips toggle → optimistic UI update, `POST
  /api/settings/logging-enabled` with new value. On failure, revert with a
  toast.
- When transitioning ON → OFF, the buttons hide immediately (no animation
  needed). The server-side `log_ring_clear()` runs as part of the POST.
- When transitioning OFF → ON, the buttons appear and the stats line begins
  showing the (empty) ring filling up.

### Button behaviours

**Download log**
```js
window.location = '/api/logs/download';
```

**Email log** — combined behaviour:
```js
window.location = '/api/logs/download';
setTimeout(() => { window.location = mailtoUrl; }, 400);
```

**Clear log** — confirm dialog ("Clear 11.2 KB of captured log data?") →
`POST /api/logs/clear` → re-poll stats.

### Stats polling

`GET /api/logs/stats` is called when Advanced opens (only if toggle is ON),
and again after a Clear. No background polling — the modal is short-lived
and live updates aren't worth the noise. Dropped count rendered in orange
when non-zero.

### Button behaviours

**Download log**
```js
window.location = '/api/logs/download';
```
Browser saves the file via Content-Disposition. Toast: "Saved
gopro-ctrl-…txt to Downloads".

**Email log** — combined behaviour per the locked decision:
```js
const url = `mailto:${SUPPORT_EMAIL}` +
  `?subject=${encodeURIComponent(subject)}` +
  `&body=${encodeURIComponent(body)}`;
window.location = '/api/logs/download';   // kick the download first
setTimeout(() => { window.location = url; }, 300);
```

**Clear log** — confirm dialog ("Clear 11.2 KB of captured log data?") →
`POST /api/logs/clear` → re-poll stats.

### Email body template

Recipient: `imdacrocker@gmail.com` (hard-coded for v1; promote to Kconfig
`CONFIG_SUPPORT_EMAIL` if a second target ever appears).

Subject: `GoPro Controller log — <mac6> — fw <version>`

Body (the `\n`s render as line breaks in the user's mail client; the bold
markdown won't render but the ALL-CAPS and spacing make it visible enough):

```
WHAT I WAS TRYING TO DO:


WHAT HAPPENED:


WHAT I EXPECTED INSTEAD:


------------------------------------------------------------
>>> PLEASE ATTACH THE LOG FILE THAT WAS JUST DOWNLOADED <<<
    (look in your Downloads folder for a file named
     gopro-ctrl-<mac6>-<timestamp>.txt)
------------------------------------------------------------

PRIVACY NOTE: the attached log file may contain device
identifiers (MAC addresses of cameras paired with this
controller, SSIDs of nearby GoPro WiFi networks, and the
controller's own network name). It does not contain WiFi
passwords or location data. Please remove anything you
consider sensitive before sending.

Firmware: <version> (<channel>)
Device:   <mac>
Sent:     <utc or uptime>
```

The fields after the dashed line are filled in client-side from
`/api/version` and `/api/utc` values the UI already holds.

---

## 9. Privacy considerations

The log will, by default, contain:

- Bluetooth MAC addresses of any GoPro paired or discovered during a scan.
- WiFi SSIDs of GoPro cameras attempting to join the SoftAP (Hero 4 flow).
- The controller's own AP SSID.
- Camera model strings, firmware versions reported by GoPros.
- Timestamps (UTC or uptime).
- Internal protocol traffic at the level of "what command was sent / what
  response came back" — not raw GATT payloads unless DEBUG enables it.

The log will **not** contain:

- WiFi passwords. `wifi_manager` does not log the PSK.
- GPS coordinates. `can_manager` logs the fact that UTC was acquired, not
  position fixes.
- User PII — the firmware has none.

The email body warns the user about MACs and SSIDs and tells them to redact
before sending. A pre-shipping pass over every `ESP_LOGx` call site should
confirm no secret material ever reaches the formatter. This is a one-time
audit, not a recurring task — but worth re-running after large refactors.

**Kill switch.** The "Enable Logging" toggle in the Advanced Settings modal
lets a privacy-conscious user disable capture entirely. Toggling OFF clears the ring
immediately and persists the preference across reboots, so once off it stays
off until the user opts back in. UART output is unaffected (developer
console).

---

## 10. Sizing & cost

| Resource | Cost |
|---|---|
| RAM (static) | 64 KB ring + ~40 B mutex + ~50 B counters ≈ 64.1 KB. **Hard ceiling on internal-DRAM rings is ~64 KB on this build** — 128 KB starved the heap. Bigger needs PSRAM. |
| RAM (heap, transient) | None — downloads stream from a 2 KB stack buffer |
| Flash (code) | log_ring + api_logs ≈ 2 KB |
| Flash (DEBUG strings) | +1–3 KB across all components, varies with how chatty existing code is |
| CPU | One memcpy + mutex pair per log line. Negligible relative to existing serial output. |

The static array is visible to `idf.py size` so RAM regressions show up at
build time. If `apps/wireless` ever runs tight on internal SRAM, options in order
of preference: lower `LOG_RING_SIZE_KB` to 32; switch the ring to PSRAM if a
hardware module with PSRAM gets adopted; gzip the download to allow a smaller
ring without sacrificing capture history.

---

## 11. Failure modes

| Condition | Behaviour |
|---|---|
| Mutex take fails in hook | Line dropped, `lines_dropped_total` incremented, UART echo still attempted. |
| Browser cancels download mid-stream | `httpd_resp_send_chunk` returns non-OK, `log_ring_stream` propagates it back, handler terminates the chunked response. No heap to clean up. |
| Ring fills during a busy moment | Oldest whole lines evicted to make room. `lines_dropped_total` ticks up. The UI surfaces this as the orange "dropped" indicator so testers know context was lost. |
| Writes evict bytes during a download | Stream skips ahead past the evicted absolute byte range. Result: a slightly-truncated file, never duplicated content, never a malloc failure. |
| `ESP_EARLY_LOGx` from boot ROM | Not captured (uses pre-vprintf path). Acceptable — these are static strings from the bootloader, not interesting for app diagnostics. |
| Log line >256 bytes | Truncated at 256. Truncation is silent. |
| Logging disabled mid-session | Ring cleared as part of the same call. An in-flight download stream that already snapshotted the end-of-range will keep sending until that range is reached; new requests see an empty ring. |
| NVS read fails at boot | Treat as `enabled=true` (default). Logged via `ESP_LOGW` from `api_settings`. |
| NVS write fails on toggle | API returns 500, UI reverts the toggle and shows a toast. The in-memory flag is **not** updated, so behaviour stays consistent with what NVS holds. |

---

## 12. Open questions / deferred

- **Per-tag filtering at runtime.** Would let testers temporarily silence a
  noisy module without a rebuild. Cheap to add later via a `POST
  /api/logs/level` endpoint.
- **`esp_core_dump` integration.** Would survive panics by writing the tail
  of the ring to the coredump partition. Requires a partition table change;
  best bundled with any other partition rework.
- **gzip on download.** Worth adding the first time a real-world report
  attachment fights the user's mail client size limit.
- **Configurable recipient.** Currently hard-coded. Promote to Kconfig the
  first time a second target appears.
- **Mobile Web Share API.** `navigator.share({ files: [...] })` could enable
  true one-tap "share log via email app" on Android and iOS — but with
  enough caveats (HTTPS-only on some browsers, inconsistent email-app
  targeting) that it's not worth the complexity for v1.

---

## 13. Implementation order

1. Create `log_ring` component: `include/log_ring.h`, `log_ring.c`, `Kconfig`,
   `CMakeLists.txt`. Owns the ring, the vprintf hook, the enable flag, **and
   its own NVS persistence** (namespace `logring`, key `enabled`).
2. Wire `log_ring_init()` as the first call in `app_main`, and
   `log_ring_load_persisted_enabled()` immediately after `nvs_flash_init()`.
   Add `log_ring` to `apps/wireless/main/CMakeLists.txt` REQUIRES.
3. **sdkconfig:** existing defaults already pair `LOG_DEFAULT_LEVEL_INFO=y`
   with `LOG_MAXIMUM_LEVEL_VERBOSE=y` — no change needed.
4. Add `api_logs.c` (three endpoints), register from `driver.c`, bump
   `max_uri_handlers`. Add `log_ring` to `http_server` REQUIRES.
5. Add `GET`/`POST /api/settings/logging-enabled` to `api_settings.c` —
   these are thin wrappers around `log_ring_is_enabled()`,
   `log_ring_set_enabled()`, and `log_ring_save_enabled_to_nvs()`.
6. One-time audit pass over `ESP_LOG*` callers for secret material.
7. Add the Logging section inside the Advanced Settings modal (web UI): the
   toggle, the conditionally-rendered stats line, and the three buttons.
8. Update [`web-ui.md`](web-ui.md) §12.3 to reflect the new Advanced modal,
   and update
   [`memory/project_http_server.md`](../../memory/project_http_server.md)
   with the new endpoints and `max_uri_handlers` value.

**Status as of 2026-05-17:** firmware + web UI both implemented. Capture
ON/OFF, download, email handoff, and clear all functional. Field-tested with
a HERO13 Black; ring volume calibrated against real captures (silenced
tags: `httpd*`, `vfs_calls`, `efuse`, `intr_alloc`, `temperature_sensor_hal`,
`nvs`, `event`).

Steps 1–5 are firmware (this PR). Step 6 is housekeeping. Steps 7–8 are
the web-UI follow-up after the API has been exercised in dev.
