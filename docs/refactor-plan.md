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

- [x] **CI build gate**: `.github/workflows/ci.yml` builds `apps/wireless` and
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

- [x] **camera_manager.c:921 `reorder_slots`** — when `count != s_slot_count`
      the tail slots were left untouched in RAM/NVS while indices [0, count)
      got overwritten, silently dropping old slot 0 and duplicating the tail
      across two slots; duplicate indices in `new_order` had the same effect.
      **Resolved** by extracting a pure permutation-validator
      (`reorder_validate.c` + `reorder_is_valid_permutation`) that rejects
      anything that isn't a permutation of [0, s_slot_count) — wrong count,
      out-of-range, or duplicates. The endpoint isn't called by the shipped
      UI today, so this is hardening against future use / direct API access.
      Deletion has its own path (`camera_manager_remove_slot`) — keeping the
      two operations cleanly separate. **Not user-reachable today**: no
      front-end calls `/api/reorder-cameras`. Added Unity host test
      (`test_reorder_validate.c`, 17 cases) covering accept/reject scenarios.
      Also moved `CAMERA_MAX_SLOTS` from `camera_manager.h` (ESP-IDF deps)
      to `camera_types.h` so pure-logic compilation units can reach it.
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
- [~] **open_gopro_ble/status.c:136** — band-status query bridge uses module
      globals matched only by status ID, not slot/conn_handle; in principle a
      response can satisfy the wrong waiter with 2+ cameras. **Verified not
      reachable in practice**: the only caller is `pair_complete_task`, which
      is single-flight under `pair_complete.c`'s gate (now backed by
      `s_gate_lock` + `s_pending[]` queue). Documented the load-bearing
      assumption inline at status.c:135 so a future maintainer can't quietly
      add a second caller without realising. **Per-slot refactor deferred**
      until/unless a non-pair_complete caller is needed (e.g. a UI-initiated
      "re-check WiFi band" action).
- [x] **open_gopro_ble/pair_complete.c:371** — single-flight gate silently
      *dropped* a second concurrent pair-complete request rather than queuing
      it. Reachable on first-pair of two cameras booted together: the second
      camera's BLE/readiness pipeline can complete while the first's
      pair_complete_task is still running (the scanner resumes after CCCD
      subscription, before readiness polling). **Resolved** by adding a
      mutex-protected gate + `s_pending[CAMERA_MAX_SLOTS]` queue; the dying
      task now drains pending slots via `release_busy_and_drain()` instead
      of dropping them. Deferred slots that disconnect before their turn
      are silently skipped (logged).
- [x] **open_gopro_ble/pair_complete.c:77** — global BLE-read bridge had no
      guard against late callbacks: a read whose `xSemaphoreTake` timed out
      left a pending `ble_gattc_read` in NimBLE; when that callback eventually
      fired it overwrote the bridge globals and signalled the semaphore,
      poisoning whatever read came next.
      **Resolved** with an atomic generation tag (`atomic_uint_least32_t
      s_read_gen`): each call to `read_handle_blocking` bumps the counter
      and passes the post-increment value through NimBLE's `cb_arg`; the
      callback compares its tag against the current generation and drops
      itself unconditionally on mismatch — no globals write, no signal.
      Combined with a defensive drain at entry, the bridge is now correct
      under all timing interleavings, including the cross-task interleaving
      enabled by the new pair_complete queue. NimBLE has no public "cancel
      pending read" API, so this is the right shape (vs. trying to abort
      the in-flight read). Not reachable today (first-pair-only + UI serial-
      izes pairing) but the queue I added in the previous round makes it
      potentially reachable for any future batched first-pair flow.
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
- [~] **log_ring.c:135** — flagged as a UART leak of DEBUG/VERBOSE under
      `CONFIG_LOG_COLORS=y`, but **verified not actually broken** in ESP-IDF
      v6.0.1 (the toolchain pinned in `dev.ps1`). The macros `LOG_COLOR_D` and
      `LOG_COLOR_V` in `components/log/include/esp_log_color.h` are
      unconditionally defined as `""` (empty strings) in both the
      `!ESP_LOG_COLOR_DISABLED` and `ESP_LOG_COLOR_DISABLED` branches — only
      `LOG_COLOR_I` / `LOG_COLOR_W` / `LOG_COLOR_E` carry actual ANSI escapes.
      So DEBUG/VERBOSE lines start with the level letter and the `buf[0] !=
      'D' && buf[0] != 'V'` test correctly identifies them; the in-code
      comment at log_ring.c:131-134 has the right model. **Latent risk**: if
      a future ESP-IDF defines `LOG_COLOR_D` non-empty (e.g. adds a gray
      colour for debug) the check would silently break with no compile-time
      signal. Could be hardened with a `_Static_assert(sizeof(LOG_COLOR_D) ==
      1, ...)` in log_ring.c; deferred — current toolchain is fine and the
      assert is easy to add later if the failure ever surfaces.

---

## Phase 3 — Reliability / concurrency (MEDIUM)

- **Timer-vs-callback races** — three independent sites, separated here so
  each can be tracked independently:
    - [x] **camera_manager.c:1024 `poll_timer_cb`** — classic dispatch-after-
          unlock TOCTOU. The callback null-checked `sl->driver` under the lock,
          then snapshotted `drv`/`ctx`, then unlocked, then called
          `drv->start_recording(ctx)`. `camera_manager_remove_slot` could null
          `sl->driver` and free `driver_ctx` between the snapshot and the call,
          producing a NULL deref or use-after-free. Reachable on any
          remove-slot or WiFi-disconnect that lands inside a recording-mismatch
          poll tick. **Resolved**: hold the (recursive) lock through dispatch.
          The driver methods are async on both BLE and WiFi-RC drivers — they
          queue work to their respective task queues and return quickly — so
          the global lock is held only briefly. The two post-dispatch
          `s_slots[slot].grace_until_us` writes fold into the same locked
          section.
    - [x] **open_gopro_ble/readiness.c** double-completion: three one-shot
          `esp_timer` callbacks (`third_party_timeout_cb`, `cam_ctrl_timeout_cb`,
          `on_readiness_timer`) and their matching response handlers each
          checked a `pending` bool then acted, with no atomic check-and-clear.
          Both can pass the gate (host task reads true; timer task reads true
          before host clears), and both call into the completion path —
          duplicate state transitions, duplicate `camera_manager_on_camera_ready`
          calls. **Resolved**: converted `readiness_polling`, `cam_ctrl_pending`,
          `third_party_pending` to `atomic_bool` and replaced `check + clear`
          at every claim point with `atomic_exchange(&flag, false)` — exactly
          one caller observes true→false and proceeds. Subtle case: the
          readiness retry-timer's retry path is NOT a claim (the polling
          continues), only the retry-exhausted path is — that one's now
          `atomic_exchange`-guarded too so it can't race a concurrent success
          response into double-completion. `pairing.c:183` (disconnect
          cleanup) also picked up `atomic_store`.
    - [x] **gopro_wifi_rc/connection.c** timer-after-stop: `esp_timer_stop()`
          returns synchronously but does NOT wait for an already-dispatched
          callback to drain — the keepalive/WoL-retry callback can fire after
          disarm, post `RC_CMD_KEEPALIVE_TICK`/`RC_CMD_WOL_RETRY` to the work
          queue against a torn-down slot, and the handler blindly sends UDP
          to `ctx->last_ip`. Initially considered a `wifi_ready` gate but
          rejected — `wifi_ready` is legitimately false during the initial-
          probe window (between station-associated and first UDP response)
          and gating there would break the cv/keepalive probe. **Resolved**
          at the source by calling `esp_timer_stop() + esp_timer_delete()` in
          both `rc_disarm_keepalive_timer` and `rc_disarm_wol_retry_timer`;
          `delete()` (on the default ESP_TIMER_TASK dispatch) blocks until
          any pending callback finishes, so after disarm no stale tick can
          land. The arm functions already re-create from NULL.
- [~] **Unlocked shared state** — flagged as four sites but **verified that
      none are live bugs** under current callers:
    - `wifi_manager.c:309` `s_sta_busy` TOCTOU is latent: the only callers
      of `wifi_manager_sta_join`/`wifi_manager_sta_leave` are inside
      `pair_complete_task`, which is itself single-flight under
      `pair_complete.c`'s `s_busy`+`s_gate_lock`+`s_pending[]` gate (the
      pair_complete-queue commit). Documented the load-bearing
      single-flight assumption at the `s_sta_busy` declaration so a future
      maintainer adding a non-pair_complete caller (UI WiFi scan/connect,
      CLI sta-join, etc.) gets a clear "this needs to become atomic" note.
    - `camera_manager.c:612` `s_slot_count` read in `get_slot_count` is a
      single 32-bit aligned load (atomic on ESP32-S3 by hardware). The
      Phase 0 slot-count-semantics work already documented the return
      value as an iteration upper bound; "stale by one tick" is the
      documented contract, not a bug.
    - `camera_manager.c:797-798` `s_auto_control` getter/setter is an
      aligned `bool` (atomic single-byte). Worst case is stale-by-one
      polling cycle (microseconds). Cosmetic.
    - `camera_manager.c:188` driver-registration table write is outside
      the lock by intent. Verified init order in `app_main` is sequential
      (`camera_manager_init` → `gopro_wifi_rc_init` → `open_gopro_ble_init`)
      before any other task spawns. Added an init-time-only-contract comment
      block at the function header so any future runtime-registration call
      site gets a clear "move the write under the lock" note.
    - **No code change** beyond the two documentation blocks. Original plan
      entry was an over-eager scan that didn't trace callers.
- [x] **http_server `read_body`** (http_server_internal.h:29) — single `recv`
      assumed the whole body arrived in one chunk; under TCP segmentation any
      POST body could truncate. **Resolved** by looping `httpd_req_recv` until
      `content_len` bytes have been accumulated, matching the established
      pattern in `api_ota.c:pump_body` (OTA streams) and recovery's
      `recovery_http.c:read_body_capped`. Reachable under WiFi congestion on
      the ~512-byte CAN config POST (`api_settings.c:251`); the 64–128 B
      endpoints were unlikely to segment but the fix is in the shared helper
      so all 12 POST call sites are covered without per-handler changes.
      **NB**: an earlier version of this fix also retried on
      `HTTPD_SOCK_ERR_TIMEOUT` (mirroring `pump_body`), which is appropriate
      for multi-MB OTA streams but wrong for short JSON bodies — a malicious
      client sending no bytes would have held the socket forever. The
      timeout-retry was removed in the follow-up commit; for read_body any
      `n <= 0` (timeout, peer close, error) now correctly returns 500 and
      releases the socket. The 5 s recv_wait_timeout from
      `HTTPD_DEFAULT_CONFIG` (which is the default, not 0 as the original
      commit message implied) bounds the worst-case stall.
- [~] **http_server OTA session statics** (api_ota.c:35) — flagged as
      `s_app_uploaded`/`s_ui_uploaded` "shared across concurrent workers",
      but **verified there are no concurrent workers**: ESP-IDF httpd is a
      single-task select-based server, so handler_upload_app /
      handler_upload_ui / handler_commit are serialised by httpd's worker
      itself. Client retries while a previous upload is mid-stream queue at
      TCP; the second handler invocation starts after the first returns.
      No mutex needed. Documented the load-bearing single-task assumption
      inline at the static declarations so any future move to a thread-
      pooled httpd gets an explicit "must add mutex here" pointer.
- [x] **http_server socket timeouts** (driver.c:232) — flagged as "no
      `recv_wait_timeout`/`send_wait_timeout`", but **verified that ESP-IDF
      v6.0.1's `HTTPD_DEFAULT_CONFIG()` already sets both to 5 s by default**
      and the wireless app retains that default. The "no timeout" framing in the
      plan entry was wrong. The real gap was **slow-trickle slowloris**: a
      malicious client sending one byte every 4 s keeps each `httpd_req_recv`
      under the 5 s timeout and holds the socket for ~30+ minutes per
      request; eight such clients exhaust `max_open_sockets`. **Resolved** by
      setting `config.lru_purge_enable = true` in driver.c — when all 8
      sockets are in use and a 9th client connects, ESP-IDF evicts the
      least-recently-used socket, breaking slowloris's grip. Trade-off: a
      legitimate slow client could be evicted under burst load; not realistic
      for a single-user controller. Also fixed a regression in the previous
      `read_body` commit (see above) that made the slowloris-with-zero-bytes
      case strictly worse.
- [x] **main.c:89** — retried `nvs_flash_init()` return code ignored.
      **Resolved**: `ESP_ERROR_CHECK` on both `nvs_flash_erase()` and the
      retried `nvs_flash_init()`. If even a fresh erase doesn't yield a
      usable NVS partition, panic loudly rather than proceeding with every
      downstream NVS write silently failing.
- [x] **ota_io/boot_helpers.c:105 `app_desc_is_newer`** — sorted `__DATE__`
      strings lexically; on a `secure_version` tie (which is the default
      since no per-build bump is configured), the recovery `/api/ota/boot-main`
      endpoint could pick the OLDER of two valid OTA slots whenever the build
      months sat on opposite sides of an alphabetical-vs-calendar regression
      (e.g. `"Nov 30 2024"` vs `"Jan 15 2025"`: `'J' < 'N'` lex but Jan 2025
      newer calendar). Reachable on the recovery boot-main path; not on
      normal OTA commit (which goes through `esp_ota_set_boot_partition`).
      **Resolved** by extracting a pure-logic comparator
      (`components/ota_io/app_date_compare.c` +
      `include/app_date_compare.h`) that parses `MMM DD YYYY` and `hh:mm:ss`
      into sortable integers, handles the `__DATE__` leading-space day
      format, and treats parse failures as "equal" (refuses to guess).
      `boot_helpers.c:app_desc_is_newer` now calls into it after the
      `secure_version` check. Added a 17-case Unity host test
      (`tests/host/test_app_date_compare.c`) covering: identity, newer-year
      wins, three cross-year alphabetical regressions (Nov→Jan, Sep→Jan,
      Oct→Apr), three same-year regressions (Feb→Jan, Apr→Mar, Dec→Feb),
      day formatting (` 5` vs `15` and `31` vs `15`), time tiebreaker,
      and 5 malformed-input cases that must return 0. Also threaded
      `OTA_IO_INC` into the host-test CMake include path.
- [x] **gopro_wifi_rc — `last_ip` not cleared on disassociate** (was filed as
      `udp.c:155,178`, the actual fix site is `connection.c:rc_handle_station_disconnected`).
      `find_slot_by_ip` (udp.c:178) demuxes UDP datagrams by matching
      `ctx->last_ip == src_ip`, and already guards with `last_ip != 0`. The
      disconnect handler was clearing `wifi_ready` / `recording_status` /
      timers but NOT `last_ip`, so a UDP frame at the disconnected camera's
      old IP — including one assigned by the SoftAP to a different client —
      could be attributed to the dead slot. **Resolved**: one-line
      `ctx->last_ip = 0;` in `rc_handle_station_disconnected` after the
      existing field clears. Re-populated on the next associated/DHCP
      event.
- [x] **camera_manager.c:637** — `get_slot_info`/`get_slot_can_state`
      dereference `driver->get_recording_status` under only an
      `sl->driver != NULL` guard, on the premise that the vtable contract
      (camera_manager.h:14) declares the three core methods (start, stop,
      get_recording_status) "Always non-NULL". Current BLE and RC drivers
      populate all three, so not a live crash. **Resolved** defensively by
      adding `assert()` checks for the three required methods at the top
      of `camera_manager_register_driver` — a future driver that violates
      the contract panics at boot rather than crashing on first
      mismatch-poll dispatch.
- [x] **can_manager.c:655** — state queue depth 4 could drop a BUS_OFF-entry
      event under sustained bus errors. The TX gate (`s_can_tx_enabled`) is
      set/cleared directly by the ISR so it recovers independently, but the
      `twai_node_recover()` call lives in the queue-driven recovery_task —
      if its BUS_OFF event is dropped, the controller never re-enters
      ACTIVE. **Resolved**: deepened the queue to 8, leaving headroom for
      ~2 full ACTIVE→WARNING→PASSIVE→BUS_OFF cycles before any drop.
      Considered also gating recovery off `s_can_tx_enabled`/state polling
      directly (the plan suggestion); decided against it because the
      queue-driven path is cleaner and the deeper buffer comfortably
      covers realistic burst scenarios.

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
