/*
 * gopro_wifi_rc_spec.h — Wire-protocol constants for the GoPro WiFi Smart-Remote
 * driver.  Pure constants and command byte templates; no behavioural code lives
 * here.  See camera_manager_design.md §17 for the protocol description.
 *
 * Two transports are used:
 *   - UDP (single bound socket, local port 8383, remote 8484): keepalive,
 *     status poll, shutter — all recurring traffic
 *   - HTTP/1.0 (port 80): identify probe at pair time, plus the optional
 *     date/time set on Hero4-class cameras.  Off the critical path; a TCP
 *     RST on the legacy fallback is expected and handled silently.
 */
#pragma once

#include <stdint.h>
#include <stddef.h>

/* ---- Ports (§17.2) ------------------------------------------------------- */

#define RC_UDP_TX_PORT              8484   /* destination on camera */
#define RC_UDP_RX_PORT              8383   /* local bind; also our source port */
#define RC_UDP_WOL_PORT             9      /* broadcast magic packet */
#define RC_HTTP_PORT                80

/* ---- Keepalive (§17.2.1) ------------------------------------------------- */

/* ASCII payload, sent every RC_KEEPALIVE_INTERVAL_MS to each slot's last_ip. */
#define RC_UDP_KEEPALIVE_PAYLOAD    "_GPHD_:0:0:2:0.000000\n"

/* Keepalive ACK first byte; full reply is "_GPHD_:0:0:2:\x01" (14 B). */
#define RC_UDP_KEEPALIVE_ACK_BYTE   0x5F

/* ---- Binary command templates (§17.2.2 / §17.2.3) ------------------------ */

/*
 * Every binary packet shares a 13-byte header:
 *   bytes 0-7   : zero
 *   byte  8     : selector (documented as GET/SET in the public docs, but the
 *                 Hero3-era cameras accept byte 8 == 0 for both query and
 *                 command opcodes; we follow the Lua-verified values)
 *   bytes 9-10  : sequence/counter (camera does not enforce monotonicity;
 *                 static values per opcode work in practice)
 *   bytes 11-12 : two-character ASCII opcode
 *   bytes 13+   : opcode-specific parameters
 *
 * Templates below are copied verbatim from the working Lua reference and from
 * the ESP8266 reference sketch — both target Hero3/4 successfully.
 */

/* Status request — opcode "st", 13 bytes. Camera replies with a 20-byte
 * status frame whose decode rules are in §17.2.4. */
static const uint8_t RC_PKT_ST[13] = {
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00,             /* selector */
    0x00, 0x00,       /* counter */
    's',  't',
};

/* Shutter start — opcode "SH", parameter 0x02, 14 bytes. */
static const uint8_t RC_PKT_SH_START[14] = {
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00,             /* selector */
    0x01, 0x00,       /* counter */
    'S',  'H',
    0x02,             /* p=start */
};

/* Shutter stop — opcode "SH", parameter 0x00, 14 bytes. */
static const uint8_t RC_PKT_SH_STOP[14] = {
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00,             /* selector */
    0x01, 0x00,       /* counter */
    'S',  'H',
    0x00,             /* p=stop */
};

/* Camera version (cv) request — opcode "cv", 13 bytes.  The camera replies
 * with a variable-length frame containing length-prefixed firmware-version
 * and model-name strings (see §17.2.5).  Verified responsive on Hero7 in
 * Smart-Remote mode; exact firmware/name strings observed:
 *   "HD7.01.01.90.00" / "HERO7 Black"
 * The Hero3-era ESP8266 reference includes this opcode but never used it. */
static const uint8_t RC_PKT_CV[13] = {
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00,             /* selector */
    0x00, 0x00,       /* counter */
    'c',  'v',
};

/* ---- Status (`st`) response field offsets (§17.2.4) ---------------------- */

/* All offsets are into a 20-byte response packet. */
#define RC_RESP_OPCODE_OFFSET       11   /* bytes 11-12 echo the request opcode */
#define RC_RESP_PWR_OFFSET          13   /* 1 = camera off / sleeping */
#define RC_RESP_MODE_OFFSET         14   /* 0=video, 1=photo, 2=burst, 3=timelapse */
#define RC_RESP_STATE_OFFSET        15   /* 1 = recording (when pwr == 0) */

/* Minimum bytes required before parse_st_response indexes b13/b14/b15. */
#define RC_RESP_MIN_BYTES           16

/* ---- `cv` response field offsets (§17.2.5) ------------------------------ *
 *
 * Layout decoded from a real Hero7 reply (44 bytes total):
 *   bytes  0..7   : header (zeros)
 *   byte   8      : selector echo
 *   bytes  9..10  : counter echo
 *   bytes 11..12  : opcode echo ("cv")
 *   bytes 13..15  : reserved / response-format bytes (0x00 0x03 0x01 observed)
 *   byte  16      : length of firmware string (uint8)
 *   bytes 17..    : firmware string (no NUL)
 *   byte  16+1+fw : length of model-name string (uint8)
 *   bytes ...     : model-name string (no NUL)
 *
 * Strings are not NUL-terminated; the parser must use the length prefixes. */
#define RC_CV_RESP_FW_LEN_OFFSET    16

/* ---- HTTP paths (§17.2.6) ----------------------------------------------- *
 *
 * Identification is done over UDP (`cv` opcode); HTTP is now used ONLY for
 * date/time set, and only on cameras whose `gopro_model_supports_http_datetime`
 * predicate returns true (Hero4 Black/Silver today; tunable per-model).
 *
 * Date/time set — printf format takes 6 ints in this order:
 *   year mod 100, month, day, hour, minute, second
 * Each is URL-encoded as %XX where XX is the lowercase hex of the value.
 * Example for 2026-05-04 14:30:00 → "/gp/.../date_time?p=%1a%05%04%0e%1e%00".
 * Times are local; can_manager tz offset must be applied before formatting
 * (TODO §17.13). */
#define RC_HTTP_PATH_DATETIME_FMT \
    "/gp/gpControl/command/setup/date_time?p=%%%02x%%%02x%%%02x%%%02x%%%02x%%%02x"

/* ---- Timing (§17.6 / §17.8) ---------------------------------------------- */

#define RC_KEEPALIVE_INTERVAL_MS    3000   /* per-slot UDP keepalive period */
#define RC_WOL_RETRY_INTERVAL_MS    2000   /* per-slot WoL retry period */
#define RC_STATUS_POLL_INTERVAL_MS  5000   /* global UDP status-poll period */

/* WoL retry is armed when no response received for this long. */
#define RC_KEEPALIVE_SILENCE_MS     10000

/* ---- HTTP timeouts ------------------------------------------------------- */

/* Single-attempt timeout for both the identify probe and date/time set.
 * Applies to connect, send, and recv. */
#define RC_HTTP_TIMEOUT_MS          2000

/* ---- Wake-on-LAN --------------------------------------------------------- */

#define RC_WOL_BURST                5      /* magic packets sent per WoL event */

/* ---- Task / queue parameters --------------------------------------------- */

#define RC_WORK_TASK_PRIORITY       5
#define RC_WORK_TASK_STACK_BYTES    4096
#define RC_SHUTTER_TASK_PRIORITY    7
#define RC_SHUTTER_TASK_STACK_BYTES 4096
#define RC_UDP_RX_TASK_PRIORITY     4
#define RC_UDP_RX_TASK_STACK_BYTES  2048

#define RC_WORK_QUEUE_DEPTH         16
#define RC_SHUTTER_QUEUE_DEPTH      8

/* Number of back-to-back UDP SH packets sent for a broadcast shutter
 * (255.255.255.255).  802.11 broadcasts are unacknowledged, so a single
 * dropped frame would mean a missed start; sending three improves the odds
 * that all cameras on the AP receive at least one.  Unicast shutters send
 * once. */
#define RC_SHUTTER_BROADCAST_REPEAT 3

/* ---- Response buffer sizes ----------------------------------------------- */

/* Command-channel HTTP responses (date/time set) are tiny; we don't even read
 * the body — just the status line. */
#define RC_HTTP_CMD_RESP_MAX        256
