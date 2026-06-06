# gopro_wifi_rc

Implements `camera_driver_t` for GoPro cameras using the legacy "WiFi Smart Remote" UDP protocol — Hero3 / Hero3+ / Hero4 / Hero5 / Hero7 / Hero8 all accept it as a backwards-compatible control channel. Every recurring exchange — keepalive, status poll, shutter, identify — is a short binary UDP datagram between this device's SoftAP (src port 8383) and the camera (dst port 8484). HTTP is used **only** for the optional date/time set on cameras that run an HTTP server on their STA interface (Hero4 Black/Silver today); identification moved entirely to UDP `cv` in 2026-05 once diagnostic probes confirmed Hero5/6/7/8 don't expose HTTP in this mode.

---

## Responsibilities

- **Station lifecycle**: track GoPro cameras as they associate, get a DHCP lease, and disconnect.
- **Wake-on-LAN**: broadcast magic packet bursts when an associated camera goes silent for > 5 s.
- **UDP keepalive**: send `_GPHD_:0:0:2:0.000000\n` to each camera every 3 s (fire-and-forget).
- **UDP status poll**: send binary `st` to each camera with a known IP every 5 s; parse the response for power + recording state.
- **UDP shutter**: send binary `SH` packets (param 0x02 / 0x00). The driver exposes both per-slot unicast (mismatch poll, manual single-camera web-UI command) and a broadcast variant to `255.255.255.255:8484` × 3 (used by `set_desired_recording_all`); see "Shutter dispatch" below.
- **UDP camera-version (`cv`) identify**: sent at pair time and on every keepalive tick until the camera responds; reply is a length-prefixed firmware string + model name. Drives `gopro_model_from_name()` to set the slot's model with no HTTP involvement. The slot's name field is left blank — there is no known WiFi RC protocol path to retrieve the user-set camera name. Verified on real Hero7 hardware (response: `HD7.01.01.90.00` / `HERO7 Black`).
- **HTTP date/time** (best-effort): URL-encoded hex bytes; gated on `gopro_model_supports_http_datetime()`. No-op on every model except Hero4 Black/Silver until other models' STA-side HTTP is verified.
- **Liveness watchdog**: trigger WoL retry if `last_response_tick` ages past 5 s.

---

## Dependencies

```
REQUIRES: camera_manager, can_manager, wifi_manager, esp_timer, freertos, lwip, esp_wifi
```

**Precondition:** `camera_manager_init()` must be called before `gopro_wifi_rc_init()`.
`gopro_wifi_rc_init()` must be called before `wifi_manager_set_callbacks()` and `wifi_manager_init()` — it sets up the queues and tasks that the station callbacks post into.

---

## Transport Protocol

| Channel | Direction | Local | Remote | Notes |
|---|---|---|---|---|
| All UDP commands & replies | bidirectional | 8383 | 8484 | Single bound socket; src port 8383 is what Hero3-era cameras expect from a WiFi Remote |
| Wake-on-LAN | TX broadcast | — | 9 | 102-byte magic packet (6 × 0xFF + MAC × 16); burst of 5 |
| HTTP date/time | TX only | — | 80/TCP | HTTP/1.0; only on cameras for which `gopro_model_supports_http_datetime()` returns true |

### Wire format (binary opcodes)

```
Byte:   0  1  2  3  4  5  6  7   8     9    10    11   12    13...
       [ -- 8 bytes of zero -- ] [SEL] [ ctr_hi ctr_lo ] [opcode 2 chars] [params]
```

Bytes 9–10 are static per opcode (verified working without rolling counters). Byte 8 is documented as a GET/SET selector but Hero3-era cameras accept `0x00` for both query and command opcodes; we follow the Lua-verified values rather than the public-docs convention.

### Opcodes used

| Opcode | Byte 8 | Bytes 9–10 | Length | Purpose | Response |
|---|---|---|---|---|---|
| `_GPHD_:0:0:2:0.000000\n` | (ASCII) | — | 22 B | Keepalive | `_GPHD_:0:0:2:\x01` (14 B, first byte 0x5F) |
| `s t` | 0x00 | 0x00, 0x00 | 13 B | Status request | 20 B; b13=power, b14=mode, b15=record state |
| `S H` | 0x00 | 0x01, 0x00 | 14 B | Shutter (param 0x02 start / 0x00 stop) | 15-byte echo |
| `c v` | 0x00 | 0x00, 0x00 | 13 B | Camera-version identify | Variable; length-prefixed firmware + model name |

### Shutter dispatch

The driver registers `broadcasts_to_all = true` with `camera_manager` and supplies both pairs of vtable entries:

| Caller | Path | Destination | Repeats |
|---|---|---|---|
| `camera_manager_set_desired_recording_all` (CAN `0x600`, web UI Start All) | `start_recording_all` / `stop_recording_all` | `255.255.255.255:8484` | `RC_SHUTTER_BROADCAST_REPEAT` (3) |
| `camera_manager_set_desired_recording_slot` (web UI per-camera) | `start_recording(ctx)` / `stop_recording(ctx)` | `ctx->last_ip:8484` | 1 |
| Mismatch poll | `start_recording(ctx)` / `stop_recording(ctx)` | `ctx->last_ip:8484` | 1 |

Both paths post a `rc_shutter_cmd_t { start, ip, repeat }` to the shared shutter queue; `rc_shutter_task` reads `ip` and `repeat` from the command, so it's transport-agnostic. The 3× repeat compensates for unacknowledged 802.11 broadcast frames; at 54 Mbps the burst completes in well under 1 ms. Unicast paths send once because the underlying 802.11 link layer retries on its own.

The single-camera unicast path exists deliberately: a duplicate Start to a Hero4 that's already recording has been observed to flip it back off. By keeping mismatch corrections and per-slot manual commands targeted at a single IP, peers that may already be recording stay undisturbed.

### Status (`st`) response decode

| b13 | b15 | meaning |
|---|---|---|
| 1 | × | camera off / sleeping → `RECORDING_UNKNOWN` |
| 0 | 1 | recording → `RECORDING_ACTIVE` |
| 0 | 0 | idle → `RECORDING_IDLE` |

### Camera-version (`cv`) response decode

44-byte reference reply from a real Hero7:

```
00 00 00 00 00 00 00 00  00 00 00 63 76 00 03 01     header + opcode + reserved
0f 48 44 37 2e 30 31 2e  30 31 2e 39 30 2e 30 30     0x0f=15 + "HD7.01.01.90.00"
0b 48 45 52 4f 37 20 42  6c 61 63 6b                 0x0b=11 + "HERO7 Black"
```

Layout (after the 13-byte header echo):

| Bytes | Field |
|---|---|
| 13–15 | Reserved (`0x00 0x03 0x01` observed; not parsed) |
| 16 | uint8 firmware-string length |
| 17 .. 16+fw_len | Firmware version (no NUL) |
| 16+1+fw_len | uint8 model-name length |
| .. | Model name (no NUL) |

Decode lives in `rc_parse_cv_response()` (`status.c`). The model-name string is mapped into `camera_model_t` via `gopro_model_from_name()`; the slot's model is persisted by `rc_handle_apply_cv()` (`connection.c`). The slot's name field is intentionally left blank — the cv response carries the model identity, not a user-set device name, and there is no known WiFi RC protocol path to retrieve the latter. All three (model name, mapped enum, firmware) are logged at INFO so unrecognised model_name strings can be added to the lookup table over time.

### HTTP date/time format

```
GET /gp/gpControl/command/setup/date_time?p=%YY%MM%DD%hh%mm%ss HTTP/1.0\r\n\r\n
```

Each `%XX` is URL-encoded hex of one binary byte: year mod 100, month, day, hour, minute, second. Times are local; tz offset from `can_manager` should be applied before encoding (TODO).

---

## Public API

Header: `include/gopro_wifi_rc.h`

```c
/* Lifecycle */
void gopro_wifi_rc_init(void);

/* Station callbacks — wired by main.c via wifi_manager_set_callbacks() */
void gopro_wifi_rc_on_station_associated(const uint8_t mac[6]);
void gopro_wifi_rc_on_station_dhcp(const uint8_t mac[6], uint32_t ip);
void gopro_wifi_rc_on_station_disassociated(const uint8_t mac[6]);

/* Manual add/remove — called from http_server (POST /api/rc/add) */
void gopro_wifi_rc_add_camera(const uint8_t mac[6], uint32_t ip);
void gopro_wifi_rc_remove_camera(int slot);

/* Predicates — used by http_server for /api/rc/discovered */
bool gopro_wifi_rc_is_managed_slot(int slot);
bool gopro_wifi_rc_is_managed_mac(const uint8_t mac[6]);

/* UTC sync — called from main.c on_utc_acquired (first live UTC source this
 * session: CAN GPS frame or web-UI manual set).  Per-slot rc_send_datetime()
 * is internally a no-op until can_manager reports session-synced UTC AND
 * gopro_model_supports_http_datetime() returns true for the slot's model. */
void gopro_wifi_rc_sync_time_all(void);
```

All station callbacks and `sync_time_all` post to `s_work_queue` and return immediately — safe to call from the WiFi event task.

---

## Connection Flow

```
on_station_associated(mac)
  known RC slot, last_ip != 0  → send WoL burst, prime keepalive,
                                 arm keepalive timer
  known RC slot, last_ip == 0  → wait for DHCP

on_station_dhcp(mac, ip)
  known RC slot → store last_ip;
                  prime burst: keepalive + st + cv;
                  arm keepalive timer
                  (no HTTP probe; cv answer settles the model async)

UDP RX (first datagram from slot's IP, slot not yet ready)
  → handle_promote: set wifi_ready,
                    if cv data already in ctx → apply directly,
                    else send another cv now;
                    rc_send_datetime (no-op unless Hero4)

UDP RX (cv response, any time)
  → rc_parse_cv_response: fill ctx->parsed_model_name + parsed_firmware,
                          post CMD_APPLY_CV
  → handle_apply_cv: gopro_model_from_name → set_model + save_slot;
                     identify_attempted = true (stops cv-retry on tick)
                     (slot's name field is left blank — see "Identify" above)

UDP RX (st response)
  → rc_parse_st_response: ctx->recording_status from b13/b15

UDP RX (SH echo)
  → liveness only (refreshes last_response_tick)

handle_keepalive_tick (every 3 s, per slot)
  → rc_send_keepalive
  → if !identify_attempted: rc_send_cv
  → silence-watchdog logic for WoL retry timer

on_station_disassociated(mac)
  → clear wifi_ready, disarm timers
```

If the camera never answers `cv`, the slot stays at `CAMERA_MODEL_GOPRO_HERO_LEGACY_RC` (the default seeded by `add_camera`). All UDP control still works in that state — only the resolved model and HTTP-datetime capability check are missing.

---

## Keepalive & WoL Watchdog

```
keepalive_timer fires every 3 s (per slot, armed after association)
  → rc_send_keepalive(ip)              — UDP unicast _GPHD_ to port 8484
  → if !identify_attempted:
       rc_send_cv(ip)                  — re-probe model until camera answers
  → check last_response_tick age
      age < 5 s     → if wol_retry_timer armed: disarm
      age >= 5 s    → arm wol_retry_timer

wol_retry_timer fires every 2 s
  → rc_send_wol(ip, mac)               — magic packet broadcast burst
  → rc_send_keepalive(ip)              — keepalive unicast
  → on next response (any opcode), RX task refreshes last_response_tick;
    next keepalive_tick disarms wol_retry_timer
```

`last_response_tick` is written only by `rc_udp_rx_task` and read by the work task during keepalive tick. A 32-bit aligned `TickType_t` store is atomic on Xtensa LX7; no mutex is required. The same applies to `parsed_model_name` / `parsed_firmware` (single writer = RX task, single reader = work task).

---

## Source Files

| File | Responsibility |
|---|---|
| `include/gopro_wifi_rc.h` | Public API |
| `include/gopro_wifi_rc_spec.h` | Constants: ports, `_GPHD_` payload, opcode byte templates (`RC_PKT_ST`, `RC_PKT_SH_START`, `RC_PKT_SH_STOP`, `RC_PKT_CV`), response field offsets, timing, HTTP date/time path |
| `gopro_wifi_rc_internal.h` | Private types (`gopro_wifi_rc_ctx_t`, `rc_work_cmd_t`, `rc_shutter_cmd_t`), globals, function declarations |
| `driver.c` | Per-slot context table, work queue dispatch (`CMD_PROMOTE`, `CMD_APPLY_CV`, …), global status-poll timer, vtable, public API; `add_camera` primes the camera with keepalive + st + cv |
| `connection.c` | Station lifecycle, `rc_handle_promote` (cv-aware), `rc_handle_apply_cv`, keepalive tick (with cv-retry), WoL retry, timer arm/disarm |
| `command.c` | Shutter task; `rc_http_get` (minimal — used only by datetime); `rc_send_datetime` (HTTP) |
| `status.c` | UDP status-poll handler; `rc_parse_st_response()` (binary b13/b14/b15); `rc_parse_cv_response()` (length-prefixed firmware + model name → ctx, posts CMD_APPLY_CV) |
| `udp.c` | Single bound socket on 8383; `rc_send_keepalive`, `rc_send_st`, `rc_send_sh`, `rc_send_cv`, `rc_send_wol`; RX dispatch (0x5F / `st` / `SH` / `cv`) |
| `settings.c` | Placeholder — settings sub-commands not yet implemented |

`gopro_model_from_name()` lives in `gopro/gopro_model.c` (parent component), not here.

---

## Task Affinity

All three production tasks pinned to **core 0** to share the WiFi/lwIP stack without cross-core cache invalidation.

| Task | Priority | Stack | Role |
|---|---|---|---|
| `rc_work_task` | 5 | 4 KB | Station lifecycle, promote, apply_cv, keepalive watchdog (with cv-retry), status poll, datetime sync |
| `rc_shutter_task` | 7 | 4 KB | Shutter START/STOP — higher priority to minimise latency |
| `rc_udp_rx_task` | 4 | 2 KB | `recvfrom` on the shared 8383 socket; updates `last_response_tick`; dispatch by opcode (0x5F / `st` / `SH` / `cv`) |
| `rc_datetime` | 4 | 4 KB | One-shot HTTP date/time push to Hero4 cameras on time-sync (off the work task so keepalives keep flowing) |

---

## Known TODOs

| Location | Issue |
|---|---|
| `gopro_model.c` | `gopro_model_from_name()` lookup table is keyed off the model_name string in the `cv` response. Verified on real hardware: HERO4 Black, HERO4 Silver, HERO7 Black. HERO4 Session / HERO5 Black/Session / HERO6 Black / HERO 2018 are seeded from public docs but the actual reported strings haven't been observed yet. If the camera reports a different string, the lookup falls through to LEGACY_RC and the unexpected model_name is logged at INFO. |
| `gopro_model_supports_http_datetime()` | Currently returns true only for HERO4_BLACK / HERO4_SILVER. Extend as Hero5+ STA-mode HTTP behaviour is verified. Hero7 confirmed silent on TCP port 80 in this mode (probed 2026-05); stays excluded. |
| `command.c` `rc_send_datetime` | `CAMERA_MODEL_GOPRO_HERO_LEGACY_RC` and Hero7 have no working date/time path — the camera does not run an HTTP server on its STA interface in Smart-Remote mode. Documented limitation; UI may want to surface "camera clock not synced" to the user. |
| Future opcodes | `YY` (clock/battery), `CM` (mode change), `PW` (power off) — documented in spec but not implemented. |
