# CAN Identifier Configuration Design

**Status:** Implemented (firmware + web UI) — on branch `feature/expose_canbus_settings`
**Date:** 2026-05-27

Cross-session reference for making the four CAN frame identifiers user-configurable
from the web UI, alongside their standard-vs-extended (IDE) type. Companion to
[`camera-manager.md`](camera-manager.md) §14, [`shutdown.md`](shutdown.md), and
[`web-ui.md`](web-ui.md).

### What shipped vs. what's in §8 below

The data model (§4), validation rules (§5), API (§6), and web UI (§7) shipped
as designed. The implementation slicing in §8 was followed end-to-end. One
notable deviation:

- **§8.6 host-side Unity tests deferred.** The repo has no `tests/host/`
  scaffolding yet (camera-manager.md §23.3 lists it as a "recommended
  starting point" that hasn't been built). The validation and packing
  tests will land once that infrastructure exists; meanwhile the API
  validator is exercised by the live web UI and ad-hoc curl.

---

## 1. Goals

- Let operators match the four CAN frame IDs the controller uses to whatever
  RaceCapture (or other CAN node) is already configured to send/receive, without
  reflashing.
- Support both standard 11-bit and extended 29-bit identifiers per frame —
  vehicle networks routinely mix the two, and the user shouldn't have to live
  with our default of "all standard".
- Persist the configuration across reboots, with safe fall-back to the current
  defaults for any installation that has never touched the settings.
- Validate aggressively at the API and UI layer so a bad ID never reaches the
  TWAI driver or the RX dispatch table.

### Non-goals (v1)

- Hot-swapping IDs while the bus is running. Apply-on-reboot is enough for the
  use case (operator reconfigures once, reboots, never touches it again) and
  avoids racing the RX task against a mid-flight dispatch-table swap.
- Per-channel enable/disable. All four channels stay always-on; if a user
  doesn't want shutdown-over-CAN, they ignore the frame on the RaceCapture side.
- Changing payload formats, DLC, or TX rate. Still 1 byte for `isLogging`,
  4 bytes for camera status, 8 bytes LE for UTC, 1 byte for shutdown request;
  still 5 Hz TX for camera status.
- Multi-frame protocols (ISO-TP, J1939 PGN). The four IDs are independent
  single-frame messages; nothing here changes that.
- Configurable bitrate UI changes — bitrate already has its own row in the
  modal. This work only adds rows beneath it.

---

## 2. Summary & locked decisions

| Topic | Decision |
|---|---|
| Configurable channels | Four: `logging_cmd` (RX), `cam_status` (TX), `gps_utc` (RX), `shutdown_req` (RX) |
| Per-channel fields | `ide` ∈ {std, ext} + numeric `id` |
| Defaults | `0x600`/`0x601`/`0x602`/`0x603`, all standard 11-bit — matches today's hard-coded values |
| ID range — standard | `0x008 .. 0x7FF` |
| ID range — extended | `0x008 .. 0x1FFFFFFF` |
| Reserved low IDs | `0x000 .. 0x007` rejected (high-priority range on most vehicle networks) |
| Collision rule | The four channels must hold four **distinct** `(ide, id)` pairs |
| Apply timing | **On reboot.** New values persist immediately to NVS but the running dispatch table and TX header keep boot-time values |
| Reboot hint | Same "Reboot to apply" pattern used today for baud rate — shown after any successful POST that changes a value |
| Reset to defaults | Single button in the modal; POSTs the four default `(ide, id)` pairs as one atomic update |
| API | One combined endpoint `/api/settings/can` (GET + POST) carrying `bitrate_bps` + `channels` |
| API replaces | The existing `/api/settings/can-bitrate` GET + POST. New endpoint is a strict superset |
| NVS storage | Existing `can_mgr` namespace. One packed `u32` per channel (bit 31 = IDE flag, bits 0–28 = ID) |
| Auth | None (matches existing settings endpoints) |

---

## 3. Architecture overview

```
  browser ── GET  /api/settings/can ──┐
  browser ── POST /api/settings/can ──┤
                                      ▼
                            ┌───────────────────────┐
                            │   api_settings.c      │
                            │  validate set         │── reject 400 on collision /
                            │  (collision + range)  │   out-of-range / bad IDE
                            └──────────┬────────────┘
                                       │ all-or-nothing persist
                                       ▼
                            ┌───────────────────────┐
                            │   can_manager         │
                            │   NVS namespace       │
                            │      can_mgr          │
                            │   keys:               │
                            │     bitrate           │
                            │     ch_logging        │
                            │     ch_status         │
                            │     ch_utc            │
                            │     ch_shut           │
                            └───────────────────────┘
                                       │
                              (next boot, can_manager_init reads NVS)
                                       │
                                       ▼
                            ┌───────────────────────┐
                            │  s_channels[4] in RAM │
                            │  drives:              │
                            │    rx_task dispatch   │── replace switch with
                            │    tx_timer_cb header │   4-entry lookup table
                            └───────────────────────┘
```

The boot-time values are the source of truth for everything in `can_manager`
for the rest of that boot session. Settings POSTs only ever touch NVS — never
`s_channels` — which is what makes the apply-on-reboot model race-free.

---

## 4. Data model

### 4.1 In-memory

```c
typedef struct {
    bool     extended;   /* false = 11-bit standard, true = 29-bit extended */
    uint32_t id;
} can_channel_t;

typedef enum {
    CAN_CH_LOGGING_CMD = 0,
    CAN_CH_CAM_STATUS,
    CAN_CH_GPS_UTC,
    CAN_CH_SHUTDOWN_REQ,
    CAN_CH_COUNT,
} can_channel_id_t;

/* loaded at can_manager_init() from NVS, frozen for the boot session */
static can_channel_t s_channels[CAN_CH_COUNT];
```

### 4.2 NVS layout (`can_mgr` namespace)

| Key | Type | Bits 31 | Bits 30–29 | Bits 28–0 | Default |
|---|---|---|---|---|---|
| `bitrate` (existing) | `u32` | — | — | bps | 1000000 |
| `ch_logging` | `u32` | IDE flag | reserved 0 | id | `0x00000600` |
| `ch_status` | `u32` | IDE flag | reserved 0 | id | `0x00000601` |
| `ch_utc` | `u32` | IDE flag | reserved 0 | id | `0x00000602` |
| `ch_shut` | `u32` | IDE flag | reserved 0 | id | `0x00000603` |

Packing rationale: one key per channel (vs. two separate ide+id keys) keeps the
NVS key count down, sidesteps any consistency window where a partial write
could land between the two keys, and lets a default-fallback path
(`nvs_get_u32` returns `ESP_ERR_NVS_NOT_FOUND`) substitute the correct default
in a single branch per channel.

Bits 30–29 are reserved-zero; reject any read where they are non-zero
(`s_channels[i]` falls back to default and the malformed value is overwritten
on the next legitimate POST).

### 4.3 JSON wire format

```json
{
  "bitrate_bps": 1000000,
  "channels": {
    "logging_cmd":  { "ide": "std", "id": 1536 },
    "cam_status":   { "ide": "std", "id": 1537 },
    "gps_utc":      { "ide": "std", "id": 1538 },
    "shutdown_req": { "ide": "std", "id": 1539 }
  }
}
```

- `ide`: string, one of `"std"` or `"ext"`. (String chosen over bool for
  legibility in curl debugging — `"std"` self-documents what `false` would not.)
- `id`: integer, decimal in the JSON (cJSON has no native hex support).
  The UI converts the user's `0x600` text input to/from decimal at the form
  boundary.
- Channel keys are stable strings; backend treats unknown keys as a 400.

POST accepts partial bodies — any subset of `bitrate_bps` and `channels.*`. Any
channel object inside `channels` must carry **both** `ide` and `id`. Validation
runs on the resulting merged set before any value is written.

---

## 5. Validation rules

Run in this order; first failure returns 400 with a specific message:

1. **Shape.** JSON parses, top-level object, recognised keys only.
2. **IDE string.** Each `ide` field is `"std"` or `"ext"`. Else `"invalid ide for <channel>"`.
3. **ID type.** Each `id` field is a non-negative integer. Else `"invalid id for <channel>"`.
4. **ID range.** `id ≥ 0x008` and (`ide == "std"` → `id ≤ 0x7FF`) or
   (`ide == "ext"` → `id ≤ 0x1FFFFFFF`). Else
   `"<channel> id out of range for ide"`.
5. **Bitrate.** If present, must be in the existing allow-list
   (50000, 100000, 125000, 250000, 500000, 1000000). Else `"invalid bitrate_bps"`.
6. **Collision.** All four `(ide, id)` pairs distinct after the merge. Else
   `"channel id collision: <a> and <b>"`.

A POST that fails at any step persists nothing — all-or-nothing semantics. The
existing per-channel hardware checks (TWAI driver rejects bad IDE/ID at frame
build time) stay in place as a belt-and-braces defence, but should never fire
because the API gate catches everything first.

---

## 6. HTTP API

### 6.1 `GET /api/settings/can`

Always returns the full set, even when nothing has ever been changed (the
defaults). Used by the settings modal on open and by external tooling.

```json
{
  "bitrate_bps": 1000000,
  "channels": {
    "logging_cmd":  { "ide": "std", "id": 1536 },
    "cam_status":   { "ide": "std", "id": 1537 },
    "gps_utc":      { "ide": "std", "id": 1538 },
    "shutdown_req": { "ide": "std", "id": 1539 }
  }
}
```

### 6.2 `POST /api/settings/can`

Body: any subset of the GET schema. Missing fields are read from current NVS
(or default) values to form the merged set that validation runs against.

Success: `200 {}`, plus a fresh GET-shaped response in the body so the UI can
re-render without a follow-up GET.

Errors:

| Code | Cause |
|---|---|
| 400 | Validation failed (see §5). `error` field carries a specific message. |
| 503 | `reject_if_shutting_down()` — same gate every settings POST uses. |

### 6.3 Migration

The existing `GET /api/settings/can-bitrate` and `POST /api/settings/can-bitrate`
handlers are **removed**. There are no third-party callers (this is a SoftAP
device with a single first-party web UI), so a hard switchover is safe. The
frontend changes in §7 land in the same release as the backend change.

---

## 7. Web UI

### 7.1 Modal layout

Extends the existing CAN-BUS Settings modal at
[`index.html:118`](../../apps/main/web_ui/index.html). The new rows sit beneath
the existing baud-rate row, each laid out the same way as other settings rows
(label on the left, controls on the right):

```
┌─────────────────────────────────────────────────────────┐
│  CAN-BUS Settings                              [Back]   │
├─────────────────────────────────────────────────────────┤
│  CAN Baud Rate              [ 1 Mbps        ▾ ]         │
│                                                         │
│  Logging Command (RX)       [ Standard ▾ ] [ 0x600 ]    │
│  Camera Status (TX)         [ Standard ▾ ] [ 0x601 ]    │
│  GPS UTC (RX)               [ Standard ▾ ] [ 0x602 ]    │
│  Shutdown Request (RX)      [ Standard ▾ ] [ 0x603 ]    │
│                                                         │
│  Reboot to apply new CAN settings.                      │
│                                                         │
│  [ Reset to defaults ]                                  │
└─────────────────────────────────────────────────────────┘
```

### 7.2 Input behaviour

- **IDE dropdown** — two options, `Standard` and `Extended`. Changing it
  re-validates the adjacent ID field against the new range.
- **ID input** — text input, must start with `0x` and contain only hex digits.
  Inline error shown in red beneath the row on blur for invalid input or
  out-of-range value. Submit is deferred until blur — typing `0x60` mid-edit
  does not flash an error.
- **Hex-only entry.** Decimal entry is rejected at the input. The display
  always renders as `0x…` regardless of the JSON value the server returned.
- **Live collision check.** When a row's `(ide, id)` matches another row's,
  both rows get a red border and a single error line beneath the modal
  ("Logging Command and GPS UTC share `0x600` — IDs must be distinct").
- **Submit gating.** The modal POSTs each field independently on blur, the
  same as the baud-rate row does today. If validation passes, the reboot hint
  appears. A field that fails validation never posts, so a typo doesn't
  partially corrupt the saved set.

### 7.3 Reset to defaults

Single button at the modal footer. Confirms with a small inline modal
("Restore default CAN identifiers? Reboot to apply."), then POSTs all four
channels in one body. UI re-renders to show the defaults, plus the reboot hint.

### 7.4 Reboot hint

Already exists as `#can-bitrate-hint`. Generalised in this change to
`#can-settings-hint` ("Reboot to apply new CAN settings.") and shown when any
of the five fields (bitrate + four channels) differs from the value the modal
was opened with.

---

## 8. Implementation plan

Smallest viable slices, in order. Each is one PR-sized change.

### 8.1 can_manager — channel struct + NVS

- Add `can_channel_t`, `can_channel_id_t`, `s_channels[CAN_CH_COUNT]`.
- Add `can_manager_get_channel(can_channel_id_t)` and
  `can_manager_set_channel(can_channel_id_t, can_channel_t)`.
- `set` validates range + IDE, returns `ESP_ERR_INVALID_ARG` on bad input.
  Collision-checking is the **caller's** responsibility (the API handler does
  it across all four before calling any setter — see §8.4).
- Load/save helpers using the packed `u32` format from §4.2.
- `can_manager_init()` calls a `load_channels()` helper that populates
  `s_channels` from NVS, substituting defaults for missing keys.

No behaviour change yet — the existing `#define CAN_ID_*` constants stay in
place. This slice just establishes the storage.

### 8.2 can_manager — wire dispatch through s_channels

- Replace `switch (item.id)` in `rx_task` with a small lookup:
  ```c
  for (int i = 0; i < CAN_CH_COUNT; i++) {
      if (item.id == s_channels[i].id &&
          item.ide == (s_channels[i].extended ? 1 : 0)) {
          dispatch_table[i](&item);
          return;
      }
  }
  ```
  where `dispatch_table` is a static array of the existing three handlers
  (logging / utc / shutdown) plus `NULL` for the TX-only `cam_status` slot.
- `tx_timer_cb` reads `s_channels[CAN_CH_CAM_STATUS]` and sets
  `frame.header.id` and `frame.header.ide` accordingly.
- Delete the `CAN_ID_*` macros.

After this slice, behaviour is identical because `s_channels` still holds the
defaults — but the plumbing is now data-driven.

Note: `can_rx_item_t` currently stores only `id` and `dlc` — extend it with
`bool extended` from `frame.header.ide` in `on_rx_done_isr` so the RX task can
disambiguate. Without this, a standard `0x600` and an extended `0x600` would
both dispatch to the logging handler.

### 8.3 http_server — new combined endpoint

- New handler `handler_get_can` and `handler_post_can` in
  `api_settings.c`. Both touch only NVS via `can_manager_set_channel` /
  `can_manager_set_bitrate` — no live RAM state changes.
- Validation per §5 lives entirely in the handler. `can_manager_set_channel`
  re-validates range + IDE as a safety net (defensive — the handler should
  never call it with bad input).
- POST body parsing follows the existing pattern in `handler_post_can_bitrate`
  (read body → cJSON_Parse → extract fields → cJSON_Delete → act).
- POST replies with the full GET-shaped body so the UI can re-render without
  a follow-up GET.

### 8.4 http_server — remove old bitrate endpoint

- Delete `handler_get_can_bitrate`, `handler_post_can_bitrate`, and the
  matching entries in `api_settings_register`'s URI table.

### 8.5 web_ui — modal extension

- Extend `index.html` with four new rows + hint generalisation + Reset
  button.
- Replace the GET in `openCanBus()` with `/api/settings/can`, render all five
  fields.
- Per-field blur handlers post to `/api/settings/can` with a one-field body.
- Live collision check runs on every change before the blur post fires; a
  collision both blocks the post and surfaces the inline error.
- Reset button confirms, posts all four channels in one body, re-renders.

### 8.6 Tests

Host-side unit tests (Unity, per `camera-manager.md` §23.3) for:

- Packed-u32 encode/decode round-trip across the four defaults and a handful
  of extended IDs (`0x10000000`, `0x1FFFFFFF`).
- Validation function: each rule in §5 has at least one accept + one reject
  case. Collision check covers (a) all-distinct pass, (b) two-way clash, and
  (c) different-ide-same-id (must be detected as collision because TWAI treats
  ide as part of the identifier — same id with different ide is allowed on
  the wire, but our dispatcher must still disambiguate; flagging it as a
  collision avoids surprising the user).

  Decision point: at §5 step 6 we treat `(std, 0x100)` and `(ext, 0x100)` as
  a collision even though the bus allows them. Easier to explain to users
  ("identifiers must be unique") than to debug a config where one of them
  silently doesn't fire.

No on-device integration test — manual verification via the modal +
RaceCapture loopback is enough for v1.

---

## 9. Edge cases

| Case | Behaviour |
|---|---|
| First boot, no NVS values | All four channels fall back to defaults; `cam_status` TX starts on `0x601` immediately. |
| NVS value with reserved bits 30–29 non-zero | Treated as missing; defaults used; first legitimate POST overwrites. Log a warning. |
| POST body sets one channel to an ID another channel already holds | 400 with the collision message; nothing persisted. |
| POST body sets `cam_status` to extended | Persists; on next boot, `tx_timer_cb` sends with `ide=1`. |
| POST body during `SHUTTING_DOWN` | 503 (existing `reject_if_shutting_down` gate). |
| User changes a setting and never reboots | The on-bus behaviour is unchanged for this boot session; the next reboot picks it up. Reboot hint stays visible. |
| User changes a setting then resets to defaults | Reset POST clears all four to defaults; reboot hint stays visible until reboot. |
| Bitrate change in the same modal session | Works the same as today — single POST, reboot to apply. The combined GET response means the UI sees the new bitrate next time the modal opens. |
| Bus already in BUS_OFF when settings POST arrives | Irrelevant — the POST only touches NVS, not the running TWAI driver. Recovery is independent. |
| Existing recording active when user POSTs | No effect on the recording — `s_channels` is frozen for this boot. Camera status TX continues on the boot-time ID/ide. |

---

## 10. Open items (deferred)

None at v1 sign-off. The non-goals in §1 are the obvious follow-ups if real
operator demand surfaces:

- Hot-swap apply (would need an RX-task quiesce + atomic swap of `s_channels`
  + dispatch-table rebuild).
- Per-channel enable/disable.
- Configurable TX rate for `cam_status`.
- Custom payload byte layout.
