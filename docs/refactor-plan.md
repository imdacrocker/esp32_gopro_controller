# Codebase Review & Refactor Plan

Living document tracking the systematic review/refactor of the firmware. Goals:
**find bugs**, **improve reliability**, **make docs match function**, and **establish testing**.

Findings come from a full read-only review of every C component plus the design
docs. Each item lists `file:line`, severity, the problem, and the intended fix.
Check items off as they land. Items not personally re-verified at authoring time
are marked _(reported)_; confirm in-source before fixing.

Severity: **CRITICAL** (data loss / crash in normal use) · **HIGH** (wrong
behavior reachable in normal use) · **MEDIUM** (reliability / edge cases) ·
**LOW** (cosmetic / doc / defensive).

---

## Cross-cutting root cause: slot-count semantics

`camera_manager_get_slot_count()` returns **`max_configured_index + 1`**
(camera_manager.c:161); gaps are left unconfigured (camera_manager.c:157).
Several callers wrongly treated it as *"number of configured cameras."* This
single mismatch underlay the paired-cameras JSON bug.

- [x] **Resolved**: header + README now document `get_slot_count()` as an
      exclusive iteration bound (callers must skip `!is_configured`); added
      `camera_manager_get_configured_count()` for true counts. Audited all 4
      callers — `api_shutdown.c:57` and `shutdown_manager.c:169` use it
      correctly as a bound; `api_cameras.c:253` (shutter `dispatched`) switched
      to the configured count. (Gap tolerance preserved intentionally — load
      compaction was *not* changed, to avoid reordering a user's slots / NVS
      namespaces.)

---

## Phase 0 — Test & CI foundation (do first; protects every later change)

- [x] **CI build gate**: `.github/workflows/ci.yml` builds `apps/main` and
      `apps/recovery` (ESP-IDF v6.0.1 / esp32s3) on every push and PR.
- [x] **Host unit-test harness** (`tests/host/`, CMake + CTest + Unity via
      FetchContent, runs on plain gcc — no ESP-IDF). Initial coverage:
      `mismatch.c` truth table (exhaustive) and `gopro_model` capability
      helpers + name lookup. Same job runs in CI.
- [ ] Extend host coverage: CAN ID packing/validation (`api_settings
      parse_channel_field`), `cv`/status response parsers, IP/MAC format+parse
      helpers, slot-count helper.
- [ ] (later) ESP-IDF Unity component tests under QEMU for stateful modules
      (`log_ring`, `camera_manager` slot lifecycle).
- [ ] (later) Regression fixtures from captured real GoPro frames for parsers.

---

## Phase 1 — Confirmed bugs (verified in-source)

- [x] **api_cameras.c:191** — HIGH — `/api/paired-cameras` leading-comma used
      `(i == 0)` but unconfigured slots are skipped with `continue`; a gap at
      slot 0 produced invalid JSON `[,{…}]`. **Fixed**: `first` flag.
- [x] **can_manager.c:195** — HIGH — `handle_logging_cmd` read `item->data[0]`
      with no DLC check; a 0-length 0x600 frame read uninitialised stack and
      could flip recording intent. **Fixed**: `if (item->dlc < 1) return;`.
- [x] **architecture.md:158-161** — DOC — described a "30 s one-shot rollback
      timer"; code disarms rollback synchronously right after `http_server_init()`.
      Boot-sequence list was also misordered (CAN/ble_core run *after* http_server).
      **Fixed**: list + prose now match `main.c`. (ota.md verified self-consistent.)

---

## Phase 2 — High-severity _(reported; verify before fixing)_

- [ ] **camera_manager.c:939 `reorder_slots`** — when `count != s_slot_count`,
      tail slots aren't cleared/torn down; a camera can be duplicated across
      slots and NVS left stale. Require `count == s_slot_count` or teardown the tail.
- [x] **open_gopro_ble/gatt.c:226** — CCCD handle assumed `val_handle + 1`; GATT
      doesn't guarantee it. **Resolved**: added a descriptor-discovery phase
      between chr-discovery and CCCD-write that runs `ble_gattc_disc_all_dscs`
      per notify chr over a precise (val_handle, end_handle] range and picks
      the first 0x2902 descriptor. `end_handle` is bounded by the next chr's
      `def_handle - 1` (back-patched in `on_chr_disc`) so no foreign-chr CCCD
      can be misattributed. The CCCD writer now uses the discovered handle
      and skips chrs where no 0x2902 was found, rather than writing to a
      guessed handle (which would silently succeed against a writable 0x2901
      User Description and leave notifications disabled). Adds ~150 ms to
      connect time (5 extra ATT round-trips); acceptable for spec-compliance
      across firmwares.
- [ ] **open_gopro_ble/status.c:136** — band-status query bridge uses module
      globals matched only by status ID, not slot/conn_handle; with 2+ cameras a
      response can satisfy the wrong waiter. Scope the bridge to the requesting ctx.
- [ ] **open_gopro_ble/pair_complete.c:76** — global BLE-read bridge; a late
      callback after timeout gives a semaphore the *next* read waits on,
      corrupting the following read. Drain before each read and/or tag by conn_handle.
- [x] **gopro_model.h + gopro_wifi_rc/connection.c:240** — verified as a
      *latent* defect, not a live bug: the model-locked UI pairing flow
      (`RC_MODELS = {Hero3, Hero4}` in app.js) never routes a Hero6/7/8 to RC,
      and Hero3/Hero4 cv-identify as RC-capable models. The only way to trigger
      it was a misuse path through the unfiltered `/api/rc/discovered` list
      (MAC-only — model unknown pre-`cv`). **Resolved** with a defensive clamp
      in `rc_handle_apply_cv` and `rc_handle_promote`: a cv-mapped model that
      fails `uses_rc_emulation()` is logged but NOT stored, so the slot stays
      `LEGACY_RC` and RC control keeps working. The rc-XOR-ble unit-test
      invariant added in Phase 0 guards the underlying premise. The transport-
      field refactor (`gopro_model.h:62-68`) is **dropped** — the dual-transport
      ambiguity it described is vestigial now that pairing is model-locked.
- [ ] **log_ring.c:135** — UART-echo level detection assumes DEBUG/VERBOSE lines
      start with the level letter, but under `CONFIG_LOG_COLORS=y` they start
      with ESC → DEBUG/VERBOSE leak to UART. Detect level after stripping ANSI.

---

## Phase 3 — Reliability / concurrency (MEDIUM)

- [ ] **Timer-vs-callback races**: open_gopro_ble/readiness.c:207,274,414
      (double-completion), gopro_wifi_rc/connection.c:34-73 (queued timer command
      runs against torn-down/reused slot), camera_manager.c:1025 dispatch-after-
      unlock TOCTOU. Marshal onto one task or guard with atomic flags.
- [ ] **Unlocked shared state**: camera_manager `s_slot_count`/`s_auto_control`
      (611, 786) and driver-registration table (188); wifi_manager `s_sta_busy`
      TOCTOU (309). Take the lock or document/relax explicitly.
- [ ] **http_server `read_body`** (http_server_internal.h:29) — single `recv`
      assumes whole body arrived; truncates under TCP segmentation. Loop until
      `content_len` received (like `pump_body`).
- [ ] **http_server OTA session statics** (api_ota.c:35) — `s_app_uploaded`/
      `s_ui_uploaded` shared across concurrent workers, no lock. Serialize / reject
      if in progress.
- [ ] **http_server socket timeouts** (driver.c:232) — no `recv_wait_timeout`/
      `send_wait_timeout`; stuck clients can exhaust all 8 sockets.
- [ ] **main.c:89** — retried `nvs_flash_init()` return code ignored; `ESP_ERROR_CHECK` it.
- [ ] **ota_io/boot_helpers.c:105 `app_desc_is_newer`** — sorts `__DATE__`
      strings lexically; can boot the *older* image on a `secure_version` tie.
      Parse the date or rely solely on `secure_version`.
- [ ] **gopro_wifi_rc/udp.c:155,178** — `last_ip` not cleared on disassociate →
      stale RX/poll attribution; `find_slot_by_ip` ambiguous. Clear on disassociate
      or gate on `wifi_ready`.
- [ ] **camera_manager.c:637** — `get_slot_info`/`get_slot_can_state` don't NULL-
      check `driver->get_recording_status`; a partially-populated vtable crashes.
- [ ] **can_manager.c:649** — state queue (depth 4) can drop a BUS_OFF-entry event
      that recovery relies on; kick recovery off the gate flag too, or deepen queue.

---

## Phase 4 — Documentation drift (LOW; fold into each component's phase)

- [ ] camera-manager.md §6.1/§9 NVS record version says 1/2; code is v3.
- [ ] camera_manager README `camera_slot_info_t` omits `first_pair_complete`,
      `wifi_associated`.
- [ ] ble_core README: buffer size (512 vs `NOTIFY_BUF_SIZE 517`), device name,
      `CONFIG_NIMBLE_MAX_CONNECTIONS` vs `CONFIG_BT_NIMBLE_MAX_CONNECTIONS`,
      `ble_core_callbacks_t` omits `is_shutdown_active`.
- [ ] open_gopro_ble README: GPBS reassembly "3 channels" but code uses 4
      (`GOPRO_CHAN_COUNT`). gatt.c:19-23 stale "no explicit MTU exchange" comment
      (pairing.c:142 does call it).
- [ ] can-id-configuration.md §2 line 71 "bits 0–28 = ID" omits reserved bits;
      §4.2 table is authoritative.
- [ ] shutdown.md API drift: `get_failed_slots()` vs shipped
      `get_failed_slots_mask()`; §4 "done_count == slot_count" assumes paired count
      but code uses high-water index.
- [ ] web-ui.md: missing `/api/reorder-cameras` & `/api/repair-camera` rows;
      undocumented `ip` field on paired-cameras; `error_code` enum omits
      `pair_complete_failed`.
- [ ] log-capture.md: `log_ring_stats_t` doc includes `oldest_uptime_ms`/
      `oldest_ts_ms` not in shipped struct; §6 shows `ESP_ERROR_CHECK(nvs_flash_init())`
      vs the erase-and-retry idiom.

---

## Suggested order

Phase 0 → Phase 1 (+ slot-count contract) → Phase 2 component-by-component
(each with tests where logic is pure) → Phase 3 → Phase 4 (folded in as each
component is touched).
