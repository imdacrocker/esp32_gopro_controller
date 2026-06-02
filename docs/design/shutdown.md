# Shutdown Feature Design

**Status:** Implemented (firmware + web UI) — on branch `feature/shutdown_command`
**Date:** 2026-05-26

Cross-session reference for the operator-initiated and CAN-initiated shutdown
flow. The user runs this when they're done with the rig and want the cameras
powered down cleanly before disconnecting the harness or cycling system power.
Companion to [`camera-manager.md`](camera-manager.md),
[`web-ui.md`](web-ui.md), and the can_manager / open_gopro_ble READMEs.

### What shipped vs. what's in §8 below

The firmware-side architecture (§3–§7) and HTTP API (§8 endpoint table)
shipped as designed. The web UI (§9) shipped in a simplified form:

- **No dedicated "Shutting down…" spinner screen.** The Shut Down button in
  Settings just disables itself and renames to "Shutting down…" while the
  page polls `GET /api/shutdown` once per second. The user sees the
  complete-overlay reveal directly when the state flips.
- **No `shutdown_state` field on `/api/paired-cameras`** for stale-tab
  detection. Each tab probes `/api/shutdown` on page load instead, which
  catches the "refreshed mid-shutdown" case without bloating the
  paired-cameras response.
- **Complete-overlay REBOOT button probes before reloading.** A naive
  `setTimeout(() => location.reload(), 3000)` raced the device — the
  browser would try to fetch while the ESP was still rebooting and the
  page never refreshed from the user's POV. The button now polls
  `/api/version` until it responds, then reloads. Same pattern as the
  disconnect overlay's `reconnectProbe`.

Otherwise the implementation tracks the design verbatim.

---

## 1. Goals

- Send a real sleep command to every paired camera when the user (or vehicle
  CAN bus) requests shutdown.
- Tear down all transport links (BLE, WiFi RC keepalive) so cameras don't get
  re-woken and the controller stops chattering on the bus.
- Park the ESP32 in an idle "pending reboot" state — CAN recording-intent
  frames are ignored, BLE reconnects are suppressed, web UI shows a clear
  "shutdown complete, press REBOOT" screen.
- Best-effort behaviour: a slow or unresponsive camera must not block the
  shutdown from completing — the user needs the REBOOT button to appear in a
  bounded time.

### Non-goals (v1)

- Persisting shutdown state across reboots. A real reboot returns to `IDLE`.
- Graceful power-down of the ESP32 itself (deep sleep, etc.). The expected
  end state is "user reboots the device" — no low-power mode.
- Re-pairing or recovery from a stuck shutdown without a reboot. The escape
  hatch is `POST /api/reboot` (existing).
- Shutdown of recovery-app cameras. Recovery has no camera_manager.

---

## 2. Summary & locked decisions

| Topic | Decision |
|---|---|
| Triggers | `POST /api/shutdown` (web UI button) **and** the CAN shutdown-request channel (default `0x603`, RaceCapture → ESP32; user-configurable per [`can-id-configuration.md`](can-id-configuration.md)) |
| CAN shutdown payload | 1 byte. Non-zero = request shutdown. Idempotent — repeated frames during `SHUTTING_DOWN`/`SHUTDOWN_COMPLETE` are no-ops |
| Camera coverage | Best-effort all. Send real sleep where supported; otherwise just drop keepalives and let auto-sleep kick in. |
| Hero9+ sleep | TLV command `0x05` on GP-0072 (Command UUID) |
| Hero7/8 sleep | Try TLV `0x05`; swallow `INVALID_PARAM` / any non-success response (unverified support) |
| WiFi RC sleep (any model) | `GET http://<cam>/gp/gpControl/command/system/sleep` over the existing SoftAP path. Applied uniformly to every slot whose driver is `gopro_wifi_rc` — Hero4 Black/Silver, Hero4 Session, Hero5/6/7, Hero 2018, and any `LEGACY_RC` fallback. Hero4/Hero5 are explicitly verified; the rest are best-effort. |
| Other / unknown models | Skip sleep command; stop keepalives only |
| Active recording | If `is_recording`, send stop-recording first, then sleep |
| Per-camera timeout | **5 s** from sequence start to slot marked done-or-failed |
| State machine (global) | `IDLE → SHUTTING_DOWN → SHUTDOWN_COMPLETE`. Not persisted in NVS. Resets to `IDLE` only via reboot. |
| CAN 0x600 (logging-state) gating | Dropped at can_manager before the recording-intent callback fires, whenever state ≠ `IDLE` |
| CAN 0x601 (camera status) TX | Stops once `SHUTDOWN_COMPLETE` is reached. Continues during `SHUTTING_DOWN` so the bus sees state transitions in real time. |
| BLE reconnect suppression | ble_core consults a new `is_shutdown_in_progress` gate before initiating a connect. Existing connections are torn down explicitly. |
| Web UI screen | Modeled on the existing "DISCONNECTED" screen. Two states: spinner + "Shutting down cameras..." (during `SHUTTING_DOWN`), then a green check + "Shutdown complete — safe to power off or reboot" + a prominent REBOOT button (during `SHUTDOWN_COMPLETE`). |
| Web UI gating | Other action endpoints (shutter, pair, add, etc.) return `503 Service Unavailable` while state ≠ `IDLE`. `POST /api/reboot` always works. |
| Confirmation modal | Yes — "Shut down the system? Cameras will stop recording and power off." with Cancel / Shut Down buttons. |
| Persistence | None. State is RAM-only. |
| Authentication | None (matches existing endpoints). |

---

## 3. Architecture overview

```
  ┌───────────────────┐
  │   Web UI button   │── POST /api/shutdown ──┐
  └───────────────────┘                        │
                                               ▼
  ┌───────────────────┐                ┌───────────────────┐
  │  CAN 0x603 frame  │── callback ──► │  shutdown_manager │
  └───────────────────┘                │                   │
                                       │  state: IDLE      │
                                       │       SHUTTING…   │
                                       │       COMPLETE    │
                                       └────────┬──────────┘
                                                │
              ┌─────────────────────────────────┼──────────────────────────┐
              │                                 │                          │
              ▼                                 ▼                          ▼
  ┌────────────────────┐         ┌──────────────────────┐    ┌─────────────────────┐
  │ per-slot shutdown  │         │ can_manager gates    │    │ ble_core suppresses │
  │  task (×N)         │         │  0x600 RX & 0x601 TX │    │  reconnect attempts │
  │                    │         │  on state            │    │  on state           │
  │ stop_recording →   │         └──────────────────────┘    └─────────────────────┘
  │ driver.sleep   →   │
  │ driver.teardown    │
  │ (5 s timeout)      │
  └────────────────────┘

  GET /api/shutdown ──► returns {state, failed_slots[]}
```

A new top-level component `shutdown_manager` owns the state machine, the
per-slot shutdown tasks, and the gates that other components consult. It
exposes:

- `shutdown_manager_request()` — idempotent; transitions `IDLE → SHUTTING_DOWN`
  and kicks off per-slot tasks. No-op in any other state.
- `shutdown_manager_get_state()` — returns the current `shutdown_state_t`.
- `shutdown_manager_get_failed_slots()` — copies the failed-slot bitmask into
  an out-param for the HTTP GET handler.
- `shutdown_manager_is_active()` — convenience for the gates (equivalent to
  `state != IDLE`).

---

## 4. State machine

```
       ┌──────┐                                ┌────────────────┐
       │ IDLE │── request() ────────────────► │ SHUTTING_DOWN  │
       └──────┘                                └────────┬───────┘
            ▲                                           │
            │                                  all slots │ done-or-failed
       (reboot)                                          ▼
            │                                ┌───────────────────┐
            └────────────────────────────────│ SHUTDOWN_COMPLETE │
                                             └───────────────────┘
```

| State | Means |
|---|---|
| `IDLE` | Normal operation. CAN frames honoured, BLE reconnects active. |
| `SHUTTING_DOWN` | Per-slot tasks are running. CAN 0x600 dropped. BLE reconnects suppressed. 0x601 still transmitting. Web UI shows spinner. |
| `SHUTDOWN_COMPLETE` | All slots done. CAN 0x600 still dropped. 0x601 TX stopped. Web UI shows REBOOT button. |

The transition `SHUTTING_DOWN → SHUTDOWN_COMPLETE` is driven by a
per-slot-done counter incremented from each per-slot task on exit (success or
failure). When `done_count == slot_count` the state flips. If no cameras are
paired at request time, the transition is immediate.

`failed_slots` is a `uint8_t` bitmask (4 cameras max → 4 bits used). A bit is
set when a per-slot task exits via the timeout path or a hard error in any
step. Surfaced in the GET response so the UI can render "Cam 2 didn't
acknowledge — power-cycle it manually" if desired (v1 may just say "some
cameras did not respond"; bitmask gives us room to grow).

---

## 5. Per-camera shutdown sequence

One FreeRTOS task per paired slot, spawned by `shutdown_manager_request()`.
Stack ≈ 4 KB, priority below the existing driver tasks so we don't preempt
in-flight BLE/UDP I/O.

```
  shutdown_slot_task(slot):
      deadline = now + 5000 ms

      if slot.is_recording:
          driver.stop_recording(ctx)
          wait up to 1500 ms (poll get_recording_status every 100 ms)
              OR until deadline

      if deadline not yet reached AND model supports a sleep command:
          driver.sleep(ctx)                          # new vtable entry, nullable
          wait briefly for ACK (driver-specific)
              OR until deadline

      driver.teardown(ctx)                           # already exists
                                                     # zeros last_ip, terminates BLE,
                                                     # stops keepalive timers

      if any step timed out or returned a hard error:
          set failed bit for this slot
      atomic_inc(done_count)
      if done_count == slot_count: state = SHUTDOWN_COMPLETE
```

### Driver vtable addition

```c
/* nullable — called by shutdown_manager to put the camera to sleep before
 * teardown. Implementation should issue the model-appropriate sleep command
 * and return ESP_OK on send (not on ACK — caller times the overall budget).
 * Return ESP_ERR_NOT_SUPPORTED if the model has no usable sleep path. */
esp_err_t (*sleep)(void *ctx);
```

Added to `camera_driver_t` in `camera_manager.h`. Existing drivers without an
implementation leave it `NULL`; the per-slot task treats `NULL` as
"skip sleep command, proceed to teardown".

### Driver-specific implementations

**`open_gopro_ble` (Hero7/8/9+)**

- New file `sleep.c` (or extend `control.c`) sends TLV `{0x01, 0x05}` to
  GP-0072 (Command UUID). Same TLV-framing helper as the existing shutter
  command.
- Hero9+ returns success (TLV `{0x02, 0x05, 0x00}`).
- Hero7 may return `INVALID_PARAM` (`{0x02, 0x05, 0x02}`) — log at DEBUG and
  return ESP_OK regardless. The keepalive stop + teardown will still take
  effect and the camera will auto-sleep within its inactivity window.
- Brief wait for the response notification (500 ms cap) so the BLE link
  isn't torn down mid-write.

**`gopro_wifi_rc` (every RC-emulation camera)**

The sleep vtable entry is wired on the RC driver unconditionally — it applies
to every slot the RC driver owns, regardless of which specific model the `cv`
identify resolved to (Hero4 Black/Silver, Hero4 Session, Hero5/6/7, Hero
2018, or the `CAMERA_MODEL_GOPRO_HERO_LEGACY_RC` fallback when `cv` never
answered). Verified on Hero4 and Hero5; sent best-effort to the rest because
the endpoint is the same across the family and there's no per-model gating
that would change the call.

- New helper sends `GET /gp/gpControl/command/system/sleep` via the same HTTP
  path used by `rc_send_datetime` (HTTP/1.0, port 80). Verified models
  return 200 OK.
- Returns ESP_OK on any 2xx; logs and returns ESP_OK on transport failure too
  (the camera was going away anyway).
- No capability predicate, no model-specific branching: if the slot is driven
  by `gopro_wifi_rc` and has a known `last_ip`, the helper runs. Models that
  silently ignore the endpoint still get their UDP keepalive stopped and will
  auto-sleep on their normal inactivity timer.

**`gopro_model.h`** — no new capability predicate needed. The presence of a
non-NULL `sleep` vtable entry is the capability.

---

## 6. CAN integration

### 6.1 New trigger: `0x603` RX

Added to the `0x600` family table in `can_manager/README.md`:

| ID | Direction | Description |
|----|-----------|-------------|
| `0x603` | RaceCapture → ESP32 | Byte 0 non-zero = request shutdown. Idempotent. |

`can_manager` exposes a new callback in `can_manager_callbacks_t`:

```c
/* Fires on 0x603 frames with byte0 != 0. Idempotent — caller is responsible
 * for de-duping at the shutdown_manager layer. */
typedef void (*can_shutdown_request_cb_t)(void *arg);
```

Wired in `main.c` to call `shutdown_manager_request()`.

### 6.2 0x600 gating

The existing 0x600 logging-state RX path (which currently calls into
`camera_manager_set_desired_recording_all` via a callback) gains a single
guard at the can_manager entry point:

```c
if (shutdown_manager_is_active()) {
    /* Drop silently — vehicle may still be transmitting after we shut down. */
    return;
}
```

Reasoning for gating at can_manager rather than camera_manager: keeps the
"shutdown" semantic out of camera_manager (which has nothing to do with CAN)
and means the existing `auto_control` flag retains its current meaning
("user wants manual control via web UI"), unrelated to shutdown.

### 6.3 0x601 TX suppression

The 5 Hz 0x601 transmit timer checks state on each tick:

```c
static void send_status_tick(void *arg) {
    if (shutdown_manager_get_state() == SHUTDOWN_COMPLETE) return;
    /* ...existing transmit logic... */
}
```

During `SHUTTING_DOWN` we keep transmitting so the bus sees slots transition
to `DISCONNECTED` as their drivers tear down. Once `SHUTDOWN_COMPLETE`, the
bus goes quiet — the RaceCapture-side observer treats CAN silence on 0x601
as the shutdown indicator.

(We do not actively send a final "all-disconnected" frame at the
`SHUTTING_DOWN → SHUTDOWN_COMPLETE` boundary; the 5 Hz cadence guarantees
the vehicle has already seen a frame with the final per-slot states within
200 ms of teardown.)

---

## 7. BLE reconnect suppression

`ble_core` currently scans + reconnects whenever
`camera_manager_has_disconnected_cameras()` returns true. A new gate is
added alongside:

```c
if (shutdown_manager_is_active()) return;
```

at the head of the reconnect-decision function. The per-slot teardown calls
already terminate the live connections; this gate prevents the reconnect
loop from immediately redialing them.

---

## 8. HTTP endpoints

New file: `apps/wireless/components/http_server/api_shutdown.c`. Registered from
`driver.c` like other `api_*_register()` calls. `http_server` CMakeLists
gains `shutdown_manager` in REQUIRES. `max_uri_handlers` grows by 2.

### `POST /api/shutdown`

- Body: none (or empty `{}`).
- Calls `shutdown_manager_request()`.
- Always returns 200 with the current state, even on a repeat call (the
  request is idempotent):
  ```json
  { "state": "shutting_down" }
  ```
- The handler returns immediately — per-slot work runs in its own tasks.

### `GET /api/shutdown`

```json
{
  "state": "idle" | "shutting_down" | "shutdown_complete",
  "failed_slots": [2, 4]      // external 1-based slot numbers; empty when none
}
```

UI polls this every 500 ms while the shutdown screen is visible.

### Other endpoints during shutdown

A small helper in `http_server_internal.h`:

```c
static inline esp_err_t reject_if_shutting_down(httpd_req_t *req) {
    if (shutdown_manager_is_active()) {
        httpd_resp_set_status(req, "503 Service Unavailable");
        send_json(req, "{\"error\":\"shutdown in progress\"}");
        return ESP_FAIL;
    }
    return ESP_OK;
}
```

Called at the top of every action handler in `api_cameras.c`, `api_rc.c`,
`api_settings.c` (POSTs only — GETs stay readable). `POST /api/reboot`
deliberately does **not** check, so it remains the escape hatch.

Read-only endpoints (`/api/paired-cameras`, `/api/version`, `/api/utc`,
`GET /api/auto-control`, `/api/logs/*`, `GET /api/shutdown`) are unaffected
— the UI may still want to display camera state on the shutdown screen.

---

## 9. Web UI

The shutdown screen replaces the home view (similar to how the
"DISCONNECTED" screen does today). It is shown whenever
`GET /api/shutdown` returns a non-idle state, and dismissed only by a real
reboot.

### Layout

**Confirmation modal** (before triggering):

```
┌─ Shut down system? ──────────────────────────┐
│                                              │
│  All cameras will stop recording and power   │
│  off. The controller will stop responding    │
│  to CAN messages until you reboot.           │
│                                              │
│         [ Cancel ]    [ Shut Down ]          │
└──────────────────────────────────────────────┘
```

**During `SHUTTING_DOWN`:**

```
┌──────────────────────────────────────────────┐
│                                              │
│            ⏳  Shutting down...              │
│                                              │
│       Stopping recording and powering        │
│       off cameras. This may take a few       │
│       seconds.                               │
│                                              │
└──────────────────────────────────────────────┘
```

**On `SHUTDOWN_COMPLETE`:**

```
┌──────────────────────────────────────────────┐
│                                              │
│            ✓  Shutdown complete              │
│                                              │
│       All cameras have been powered off.     │
│       The controller is safe to disconnect   │
│       or reboot.                             │
│                                              │
│  (if failed_slots non-empty:)                │
│       ⚠ Cam 2 did not respond — please       │
│         power it off manually.               │
│                                              │
│              [    REBOOT    ]                │
│                                              │
└──────────────────────────────────────────────┘
```

REBOOT button calls `POST /api/reboot` and then shows the standard
"rebooting…" screen the UI already has.

### Shutdown button location

A new **Shut Down** button lives in the Settings modal alongside Reboot,
styled in red. (Putting it on the main screen risks accidental taps; the
gear-menu placement matches Reboot and Restart-to-Recovery.)

### Polling

While the shutdown screen is up, the UI polls `GET /api/shutdown` every 500
ms. Stops polling once it sees `shutdown_complete` (no further state changes
possible without a reboot).

### Behaviour on stale tabs

If a second browser tab is open and the user triggers shutdown in tab A,
tab B's next poll of any endpoint will either return 503 (POSTs) or normal
data (GETs). Tab B's UI should detect the shutdown by polling
`/api/shutdown` on a low cadence (e.g. piggyback on the existing 1 Hz
paired-cameras poll — if the response contains a new top-level
`shutdown_state` field, transition to the shutdown screen). This is a small
addition to `/api/paired-cameras` to avoid every screen needing its own
poll.

(Alternative: a websocket / SSE push. Out of scope for v1 — the existing UI
is poll-based throughout.)

---

## 10. Failure modes

| Condition | Behaviour |
|---|---|
| Camera doesn't ACK sleep TLV within 500 ms | Proceed to teardown anyway. No slot-failed mark — sleep send succeeded at the BLE layer; ACK timeout is informational only. |
| Camera doesn't acknowledge stop-recording within 1500 ms | Proceed to sleep + teardown. Slot marked failed. |
| Per-slot 5 s deadline hits mid-step | Abort current step, run `driver.teardown(ctx)`, mark slot failed, increment done_count. |
| `shutdown_manager_request()` called while `SHUTTING_DOWN` | No-op. Returns ESP_OK. |
| `shutdown_manager_request()` called while `SHUTDOWN_COMPLETE` | No-op. Returns ESP_OK. (User clicks REBOOT to escape.) |
| CAN 0x603 frame arrives during shutdown | No-op. (can_manager forwards it; shutdown_manager dedupes.) |
| CAN 0x600 frame arrives during shutdown | Dropped at can_manager. No log spam — silent drop is intentional. |
| `POST /api/reboot` during `SHUTTING_DOWN` | Honoured. Cameras already received their sleep commands (or are about to time out). Best to let the user out. |
| BLE reconnect-loop runs during shutdown | Suppressed by the gate. If a connection comes in spontaneously (camera reconnect attempt), `open_gopro_ble`'s on-connect callback should check `shutdown_manager_is_active()` and immediately terminate. |
| Two tabs both POST /api/shutdown | First call transitions to SHUTTING_DOWN; second is a no-op returning the current state. Both UIs converge on the same shutdown screen. |
| Brownout / panic during shutdown | Device resets. State machine starts at IDLE. Cameras that already received sleep stay asleep; others stay awake. User reboots and the rig comes back up normally. |
| Camera was never connected (e.g. unpaired slot) | Per-slot task skips stop_recording (no `is_recording` to clear), skips sleep (driver may be NULL or return ENOENT), calls teardown (no-op), marks done without setting failed. |

---

## 11. Sizing & cost

| Resource | Cost |
|---|---|
| RAM (static) | ~80 B (state, done_count, failed_slots bitmask, mutex) |
| RAM (per-slot task) | 4 KB × up to 4 = up to 16 KB — only allocated for the duration of shutdown, freed when task exits |
| Flash (code) | shutdown_manager + driver sleep entries + api_shutdown ≈ 3 KB |
| CPU | Negligible. Per-slot tasks are mostly blocked on I/O. |

The per-slot tasks are short-lived (≤ 5 s each) so the 16 KB RAM peak is
bounded and only occurs during the operator-triggered shutdown.

---

## 12. Open questions / deferred

- **Per-camera progress in the UI.** v1 shows a single global spinner. If
  users want per-slot phase ("Cam 1: stopping recording... Cam 2: sending
  sleep..."), the per-slot task can publish its current phase to a small
  ring exposed via the GET endpoint. Cheap to add later.
- **Automatic ESP32 deep-sleep after `SHUTDOWN_COMPLETE`.** Would save
  battery in a battery-backed install. Currently out of scope — the rig is
  vehicle-powered and the user power-cycles via the harness.
- **Vehicle-side acknowledgement.** A 0x604 ESP32→RaceCapture frame
  confirming "shutdown_complete" could let the vehicle automate the
  ignition cut. Useful if 0x603 trigger sees real-world use; defer until
  there's a concrete need.
- **Cancel-mid-shutdown.** Currently no way to abort once `SHUTTING_DOWN`
  starts. Adding cancel would require re-arming BLE reconnects and
  re-opening the CAN gate; not obviously useful (the whole sequence
  finishes in ≤ 5 s anyway) so deferred.
- **Sleep ACK verification.** v1 fire-and-forgets the BLE response. If we
  ever see field reports of "camera came back on its own" we should add
  ACK gating to the sleep step.

---

## 13. Implementation order

1. **shutdown_manager component.** Create
   `apps/wireless/components/shutdown_manager/` with `include/shutdown_manager.h`,
   `shutdown_manager.c`, `CMakeLists.txt`. State machine, per-slot task
   spawner, public gate accessor.
2. **Driver vtable.** Add nullable `sleep` entry to `camera_driver_t` in
   `camera_manager/include/camera_manager.h`. No callers yet — safe to land
   independently.
3. **BLE sleep implementation.** Add the TLV `0x05` send path in
   `open_gopro_ble` and wire it into the driver's vtable.
4. **WiFi RC sleep implementation.** Add the `/system/sleep` HTTP helper in
   `gopro_wifi_rc` and wire it into the RC driver's vtable.
5. **can_manager extensions.** Add 0x603 callback type and RX dispatch;
   add `shutdown_manager_is_active()` guard at the 0x600 RX entry; add
   `state != COMPLETE` check in the 0x601 TX timer. Update
   `can_manager/README.md` and the README's 0x6xx ID table.
6. **ble_core gate.** Add `shutdown_manager_is_active()` check in the
   reconnect-decision function and in `open_gopro_ble`'s on-connect callback.
7. **HTTP endpoints.** New `api_shutdown.c` with POST + GET. Add
   `reject_if_shutting_down` helper in `http_server_internal.h` and wire it
   into POST handlers in `api_cameras.c`, `api_rc.c`, `api_settings.c`.
   Bump `max_uri_handlers` by 2. Add `shutdown_state` field to
   `/api/paired-cameras` response for stale-tab detection.
8. **main.c boot wiring.** Add `shutdown_manager_init()` after
   `camera_manager_init()` and before `can_manager_register_callbacks()` so
   the gates exist when CAN starts. Register the shutdown callback in the
   can_manager callbacks struct.
9. **Web UI.** Confirmation modal, shutdown screen (both states), polling,
   stale-tab detection. Shut Down button in Settings modal.
10. **Docs.** Update [`web-ui.md`](web-ui.md) with the shutdown screen and
    button placement; update
    [`memory/project_overview.md`](../../memory/project_overview.md) boot
    order to include `shutdown_manager_init`; update
    [`memory/project_http_server.md`](../../memory/project_http_server.md)
    with the new endpoints and `max_uri_handlers` value.

Steps 1–8 are firmware. Step 9 is the web UI. Step 10 is housekeeping.
Steps 2–4 can land in any order before step 7; step 5 depends on step 1.
