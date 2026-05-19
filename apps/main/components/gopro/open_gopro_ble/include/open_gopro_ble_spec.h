/*
 * open_gopro_ble_spec.h — Canonical BLE spec constants for Open GoPro.
 *
 * All raw byte values, packet templates, command IDs, response field offsets,
 * and GATT UUID definitions live here.  No other .c file embeds raw literals.
 *
 * Spec reference: https://gopro.github.io/OpenGoPro/ble
 */
#pragma once

#include <stdint.h>
#include "host/ble_hs.h"

/* ---- GoPro advertisement filter ----------------------------------------- */

/* 16-bit service UUID advertised by all Open GoPro cameras in scan response. */
#define GOPRO_SVC_UUID16  0xFEA6

/* ---- GoPro GATT UUID base ------------------------------------------------ */

/*
 * All GoPro characteristics share the 128-bit base UUID:
 *   b5f9XXXX-aa8d-11e3-9046-0002a5d5c51b
 *
 * NimBLE stores 128-bit UUIDs in little-endian byte order.
 * Canonical bytes:  b5 f9 XX XX  aa 8d  11 e3  90 46  00 02 a5 d5 c5 1b
 * LE (reversed):    1b c5 d5 a5  02 00  46 90  e3 11  8d aa  LO HI  f9 b5
 *
 * The XX XX suffix occupies bytes [12] (lo) and [13] (hi) of the LE array.
 * All currently used suffixes have a 0x00 high byte (range 0x0001–0x0092).
 */
#define GOPRO_UUID128_INIT(suf_lo) \
    BLE_UUID128_INIT(0x1b, 0xc5, 0xd5, 0xa5, 0x02, 0x00, 0x46, 0x90, \
                     0xe3, 0x11, 0x8d, 0xaa, (suf_lo), 0x00, 0xf9, 0xb5)

/* ---- GATT characteristic UUID suffixes ----------------------------------- */

/* Command channel */
#define GOPRO_CHR_CMD_WRITE_UUID           GOPRO_UUID128_INIT(0x72)  /* GP-0072 Write */
#define GOPRO_CHR_CMD_RESP_NOTIFY_UUID     GOPRO_UUID128_INIT(0x73)  /* GP-0073 Notify */

/* Settings channel */
#define GOPRO_CHR_SETTINGS_WRITE_UUID      GOPRO_UUID128_INIT(0x74)  /* GP-0074 Write */
#define GOPRO_CHR_SETTINGS_RESP_UUID       GOPRO_UUID128_INIT(0x75)  /* GP-0075 Notify */

/* Query channel */
#define GOPRO_CHR_QUERY_WRITE_UUID         GOPRO_UUID128_INIT(0x76)  /* GP-0076 Write */
#define GOPRO_CHR_QUERY_RESP_NOTIFY_UUID   GOPRO_UUID128_INIT(0x77)  /* GP-0077 Notify */

/* Network Management channel (carries Feature 0x03 — RequestPairingFinish, etc.) */
#define GOPRO_CHR_NW_MGMT_WRITE_UUID       GOPRO_UUID128_INIT(0x91)  /* GP-0091 Write */
#define GOPRO_CHR_NW_MGMT_RESP_NOTIFY_UUID GOPRO_UUID128_INIT(0x92)  /* GP-0092 Notify */

/* WiFi AP State (GP-0001 service) — Read/Indicate, single-byte payload
 * (0 = AP off, 1 = AP on).  Camera pushes this whenever its WiFi AP toggles. */
#define GOPRO_CHR_WIFI_AP_STATE_UUID       GOPRO_UUID128_INIT(0x05)  /* GP-0005 Indicate */

/* CCCD descriptor UUID (standard BLE) */
#define BLE_GATT_DSC_CLT_CFG_UUID16  0x2902

/* ---- GPBS packet encoding ------------------------------------------------ */

/*
 * GoPro BLE Specification (GPBS) header byte layout:
 *
 *   Bit 7    : 0 = start packet, 1 = continuation packet
 *   Bits 6-5 : (start only) 00 = general (≤31 B), 01 = ext-13, 10 = ext-16
 *   Bits 4-0 : (start, general) payload length
 *   Bits 6-0 : (continuation) sequence number
 */
#define GPBS_HDR_CONTINUATION  0x80u
#define GPBS_HDR_EXT13         0x20u
#define GPBS_HDR_EXT16         0x40u
#define GPBS_HDR_GENERAL_MAX   31u   /* max payload length for general header */

/* Maximum reassembled response buffer (longest GoPro response observed ~256 B) */
#define GPBS_MAX_RESPONSE_LEN  512u

/* ---- CCCD subscription values -------------------------------------------- */
#define BLE_CCCD_NOTIFY   0x0001u
#define BLE_CCCD_INDICATE 0x0002u

/* ---- TLV command IDs ----------------------------------------------------- */

/*
 * GetHardwareInfo (0x3C)
 * Written to: cmd_write (GP-0072)
 * Response on: cmd_resp_notify (GP-0073)
 * Spec: https://gopro.github.io/OpenGoPro/ble/features/query.html#get-hardware-info
 */
#define GOPRO_CMD_GET_HARDWARE_INFO  0x3Cu

/*
 * SetShutter (0x01)
 * Written to: cmd_write (GP-0072)
 * Response on: cmd_resp_notify (GP-0073)
 * Payload: [cmd_id, 0x01 (param len), 0x00|0x01 (off|on)]
 * Spec: https://gopro.github.io/OpenGoPro/ble/features/control.html#set-shutter
 */
#define GOPRO_CMD_SET_SHUTTER  0x01u

/*
 * SetThirdPartyClient (0x50)
 * Written to: cmd_write (GP-0072)
 * Response on: cmd_resp_notify (GP-0073)
 * TLV payload: [GPBS_hdr=1, cmd=0x50] — no parameters.
 *
 * Tells the camera that a non-official third-party app has connected.  Legacy
 * Hero5/6/7 require this for the camera to consider itself "paired with the
 * app" — without it the camera won't honour Wi-Fi-on requests and will fall
 * back to "Connect new" pairing mode on reboot.  Newer cameras accept it
 * harmlessly, so it is sent unconditionally.
 */
#define GOPRO_CMD_SET_THIRD_PARTY_CLIENT  0x50u

/*
 * SetMode (0x02) — legacy mode selection.
 * Written to: cmd_write (GP-0072)
 * Response on: cmd_resp_notify (GP-0073)
 * TLV payload: [GPBS_hdr=3, cmd=0x02, param_len=1, value]
 *   value: 0x00 = Video, 0x01 = Photo, 0x02 = Multishot
 *
 * Used only by legacy-BLE cameras (Hero7) to put the camera into video mode
 * during the initial connection sequence so subsequent shutter commands take
 * effect.  Newer cameras would use Load Preset Group (cmd 0x3E) instead.
 */
#define GOPRO_CMD_SET_MODE   0x02u
#define GOPRO_MODE_VIDEO     0x00u

/*
 * SetDateTime (0x0D)
 * Written to: cmd_write (GP-0072)
 * Response on: cmd_resp_notify (GP-0073)
 * Spec: https://gopro.github.io/OpenGoPro/ble/features/control.html#set-date-time
 *
 * Payload TLV:
 *   [0x01, 0x04, year_hi, year_lo, month, day]   (date param)
 *   [0x02, 0x03, hour, minute, second]            (time param)
 */
#define GOPRO_CMD_SET_DATE_TIME  0x0Du

#define GOPRO_DT_PARAM_DATE  0x01u
#define GOPRO_DT_PARAM_DATE_LEN  4u   /* year(2B big-endian), month, day */
#define GOPRO_DT_PARAM_TIME  0x02u
#define GOPRO_DT_PARAM_TIME_LEN  3u   /* hour, minute, second */

/* Full SetDateTime packet: header + cmd + date_TLV + time_TLV */
#define GOPRO_SET_DATETIME_PAYLOAD_LEN \
    (1u + 2u + GOPRO_DT_PARAM_DATE_LEN + 2u + GOPRO_DT_PARAM_TIME_LEN)  /* 14 bytes */

/*
 * BLE keepalive (Command ID 0x5B)
 * Written to: settings_write (GP-0074)
 * TLV payload: [GPBS_hdr=3, cmd=0x5B, param_len=1, value=0x42]
 * Period: 3 seconds
 * Spec: https://gopro.github.io/OpenGoPro/ble/features/control.html#keep-alive
 */
#define GOPRO_CMD_KEEPALIVE           0x5Bu
#define GOPRO_KEEPALIVE_VALUE         0x42u
#define GOPRO_KEEPALIVE_PERIOD_MS     3000u

static const uint8_t k_gopro_keepalive_pkt[4] = {
    0x03u,                    /* GPBS header: general, len=3 */
    GOPRO_CMD_KEEPALIVE,      /* command ID 0x5B */
    0x01u,                    /* parameter length = 1 */
    GOPRO_KEEPALIVE_VALUE,    /* parameter value = 0x42 */
};

/* ---- Query feature: GetStatusValue --------------------------------------- */

/*
 * GetStatusValue (0x13) — request specific status fields by ID.
 * Written to: query_write (GP-0076)
 * Response on: query_resp_notify (GP-0077)
 *
 * Payload TLV: [cmd_id, status_id_1, status_id_2, ...]
 * Response: [cmd_id, status, (id, len, value)*]
 *
 * Status ID 10 = Encoding (0 = idle, 1 = recording). Previously we queried
 * ID 8, but that is "Busy" per the OpenGoPro spec — it tracks transient
 * camera-busy state (menu transitions, settings writes), not the recording
 * flag, so on Hero10 it stayed 0 while recording and tripped the mismatch
 * poll into spurious SetShutter retries.
 *
 * Spec: https://gopro.github.io/OpenGoPro/ble/features/query.html#get-status-value
 */
#define GOPRO_QUERY_GET_STATUS_VALUE  0x13u
#define GOPRO_STATUS_ID_ENCODING_ACTIVE  0x0Au

/* Recording status poll cadence — matches RC-emulation poll. */
#define GOPRO_STATUS_POLL_INTERVAL_MS  5000u

/* ---- TLV command response format ----------------------------------------- */

/*
 * Command response layout (reassembled GPBS payload):
 *   [0]: command/setting ID (echoes the request)
 *   [1]: command status (0x00 = success)
 *   [2..]: optional response data (command-specific TLV parameters)
 *
 * Spec: https://gopro.github.io/OpenGoPro/ble/protocol/data_protocol.html#command-response
 */
#define GOPRO_RESP_HDR_LEN     2u
#define GOPRO_RESP_CMD_IDX     0u
#define GOPRO_RESP_STATUS_IDX  1u
#define GOPRO_RESP_STATUS_OK   0x00u

/* ---- GetHardwareInfo response body layout -------------------------------- */

/*
 * The body is a sequence of positional length-value fields (NOT TLV).
 * Each field is [len (1B), value (len B)], in this fixed order:
 *
 *   1) model number   (uint32_t, big-endian) ← maps to camera_model_t
 *   2) model name     (string)
 *   3) deprecated     (string, ignored)
 *   4) firmware       (string)
 *   5) serial number  (string)
 *   6) AP SSID        (string)
 *   7) AP MAC address (6 raw bytes)
 *
 * Parsing lives in readiness.c (parse_and_log_hw_info).
 *
 * Spec: https://gopro.github.io/OpenGoPro/ble/features/query.html#get-hardware-info
 */

/* ---- Multi-channel protobuf operations ----------------------------------- */

/*
 * Some BLE features are exposed as protobuf-encoded action commands rather
 * than TLV.  Each request and response is wrapped with a 2-byte header
 * identifying the feature and action; the rest of the payload is a protobuf
 * message defined in the OpenGoPro .proto files.
 *
 * Packet payload (after the GPBS length header):
 *   [0]: feature_id   — see GOPRO_PROTO_FEATURE_*
 *   [1]: action_id    — request: 0x69, response: action | 0x80
 *   [2..]: protobuf body
 *
 * Used here exclusively for SetCameraControlStatus on the Command channel
 * (feature 0xF1).  COHN provisioning paths (features 0x02 / 0xF5 COHN
 * actions) have been removed.
 *
 * Spec: OpenGoPro protobuf definitions at github.com/gopro/OpenGoPro/protobuf
 */
#define GOPRO_PROTO_FEATURE_COMMAND       0xF1u  /* RequestSetCameraControlStatus */
#define GOPRO_PROTO_FEATURE_NW_MGMT       0x03u  /* Network Management (PairingFinish, ...) */

/* COMMAND-feature action IDs (feature 0xF1) */
#define GOPRO_CMD_ACTION_SET_CAM_CTRL     0x69u  /* RequestSetCameraControlStatus */
#define GOPRO_CMD_RESP_SET_CAM_CTRL       0xE9u  /* ResponseGeneric */

/* NETWORK-MANAGEMENT-feature action IDs (feature 0x03) */
#define GOPRO_NW_MGMT_ACTION_PAIRING_FINISH  0x01u  /* RequestPairingFinish */
#define GOPRO_NW_MGMT_RESP_PAIRING_FINISH    0x81u  /* ResponseGeneric */

/* ---- Protobuf encoding helpers ------------------------------------------ */

/* Protobuf wire types: 0=varint, 1=64-bit, 2=len-delim, 5=32-bit. */
/* Field tag byte = (field_number << 3) | wire_type. */

/* ResponseGeneric carries field 1 = EnumResultGeneric. */
#define GOPRO_RESP_GENERIC_RESULT_TAG  0x08u  /* field 1, varint */
#define GOPRO_RESP_GENERIC_SUCCESS     0x01u  /* EnumResultGeneric.RESULT_SUCCESS */

/*
 * RequestSetCameraControlStatus protobuf body:
 *   field 1 varint: EnumCameraControlStatus
 *
 * EnumCameraControlStatus values: IDLE=0, CONTROL=1, EXTERNAL=2, COF_SETUP=3.
 * We always send EXTERNAL — declares this controller is driving the camera,
 * suppressing some on-screen UI.
 */
#define GOPRO_CAM_CTRL_PB_STATUS_TAG  0x08u  /* field 1, varint */
#define GOPRO_CAM_CTRL_EXTERNAL       0x02u

/*
 * RequestPairingFinish protobuf body:
 *   field 1 varint: result (EnumPairingFinishState)
 *   field 2 string: phoneName (must be non-empty per spec)
 *
 * EnumPairingFinishState: UNKNOWN=0, SUCCESS=1, FAILED=2.
 *
 * Spec: https://gopro.github.io/OpenGoPro/ble/features/network_management.html#set-pairing-state
 */
#define GOPRO_PAIRING_FINISH_PB_RESULT_TAG  0x08u  /* field 1, varint */
#define GOPRO_PAIRING_FINISH_PB_NAME_TAG    0x12u  /* field 2, len-delim */
#define GOPRO_PAIRING_FINISH_STATE_SUCCESS  0x01u

/* ---- Protobuf response header indices ----------------------------------- */

#define GOPRO_PROTO_RESP_FEATURE_IDX  0u
#define GOPRO_PROTO_RESP_ACTION_IDX   1u
#define GOPRO_PROTO_RESP_HDR_LEN      2u

/* ---- Readiness poll parameters ------------------------------------------- */

#define GOPRO_READINESS_RETRY_MAX     10u
#define GOPRO_READINESS_RETRY_MS      3000u

/* ---- SetCameraControlStatus timeout -------------------------------------- */

/* Time we wait for the SetCameraControlStatus ResponseGeneric before giving
 * up and proceeding with the rest of the connection sequence anyway. */
#define GOPRO_CAM_CTRL_TIMEOUT_MS     3000u

/* ---- SetThirdPartyClient timeout ----------------------------------------- */

/* Time we wait for the SetThirdPartyClient TLV response before giving up and
 * proceeding.  Hero7 may not respond to cmd 0x50 — the timeout fall-through
 * keeps the connection sequence moving in that case. */
#define GOPRO_THIRD_PARTY_TIMEOUT_MS  3000u
