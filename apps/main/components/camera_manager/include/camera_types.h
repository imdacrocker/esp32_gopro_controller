/*
 * camera_types.h — Pure C types shared between camera_manager and any
 * host-side unit-test targets (§23.2).  No ESP-IDF headers included here.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

/* ---- Model identification (§5.1) ---- */
typedef enum {
    CAMERA_MODEL_UNKNOWN            = 0,

    /* GoPro RC-emulation models — IDs match GoPro's official `model_number`
     * field as returned by the HTTP `/gp/gpControl` info JSON (cross-referenced
     * against goprowifihack/CameraCodenames.md).  Cameras self-identify via
     * the UDP `cv` opcode at pair time (see docs/design/camera-manager.md §17.2.5);
     * the camera's reported `model_name` string maps to one of these enum
     * values via gopro_model_from_name().
     *
     * HERO_LEGACY_RC is the sentinel fallback when the cv probe never gets
     * a reply or returns an unrecognised model_name.  It is placed at 999
     * to stay out of the way of GoPro's official ID space. */
    CAMERA_MODEL_GOPRO_HERO2            = 1,   /* HD2.01,  codename "HERO2"        */
    CAMERA_MODEL_GOPRO_HERO3_WHITE      = 2,   /* HD3.01,  codename "Shores"       */
    CAMERA_MODEL_GOPRO_HERO3_SILVER     = 3,   /* HD3.02,  codename "Blacks"       */
    CAMERA_MODEL_GOPRO_HERO3_BLACK      = 4,   /* HD3.03,  codename "Todos"        */
    CAMERA_MODEL_GOPRO_HERO3PLUS_SILVER = 10,  /* HD3.10,  codename "Uluwatu"      */
    CAMERA_MODEL_GOPRO_HERO3PLUS_BLACK  = 11,  /* HD3.11,  codename "Bawa"         */
    CAMERA_MODEL_GOPRO_HERO4_SILVER     = 12,  /* HD4.01,  codename "Backdoor"     */
    CAMERA_MODEL_GOPRO_HERO4_BLACK      = 13,  /* HD4.02,  codename "Pipe"         */
    CAMERA_MODEL_GOPRO_HEROPLUS_LCD     = 15,  /* HD3.21,  codename "Haleiwa"      */
    CAMERA_MODEL_GOPRO_HERO4_SESSION    = 16,  /* HX1.01,  codename "Rocky Point"  */
    CAMERA_MODEL_GOPRO_HEROPLUS         = 17,  /* HD3.22,  codename "Himalayas"    */
    CAMERA_MODEL_GOPRO_HERO5_BLACK      = 19,  /* HD5.02,  codename "Streaky"      */
    CAMERA_MODEL_GOPRO_HERO5_SESSION    = 21,  /* HD5.03,  codename "Margaret River" */
    CAMERA_MODEL_GOPRO_HERO6_BLACK      = 24,  /* HD6.01,  codename "Chopes"       */
    CAMERA_MODEL_GOPRO_HERO_2018        = 34,  /* H18.01,  codename "Smoky"        */
    CAMERA_MODEL_GOPRO_HERO_LEGACY_RC   = 999, /* sentinel — see comment above     */

    /* GoPro BLE */
    CAMERA_MODEL_GOPRO_HERO7_BLACK  = 30,
    CAMERA_MODEL_GOPRO_HERO9_BLACK  = 55,
    CAMERA_MODEL_GOPRO_HERO10_BLACK = 57,
    CAMERA_MODEL_GOPRO_HERO11_BLACK = 58,
    CAMERA_MODEL_GOPRO_HERO11_MINI  = 60,
    CAMERA_MODEL_GOPRO_HERO12_BLACK = 62,
    CAMERA_MODEL_GOPRO_MAX2         = 64,
    CAMERA_MODEL_GOPRO_HERO13_BLACK = 65,
    CAMERA_MODEL_GOPRO_LIT_HERO     = 70,

    /* Future manufacturer blocks: 1000+ */
} camera_model_t;

/* ---- BLE status (§7.2) ---- */
typedef enum {
    CAM_BLE_NONE = 0,   /* RC-emulation camera, or COHN camera not yet contacted   */
    CAM_BLE_CONNECTING, /* Any in-progress BLE work: scan, connect, bond, provision */
    CAM_BLE_CONNECTED,  /* L2 up, manufacturer-specific setup in progress           */
    CAM_BLE_READY,      /* Setup complete; held open as WiFi re-provision fallback  */
} cam_ble_status_t;

/* ---- WiFi / network status (§7.3) ---- */
typedef enum {
    WIFI_CAM_NONE = 0,  /* Not on network                                          */
    WIFI_CAM_ASSOCIATING,
    WIFI_CAM_ASSOCIATED,
    WIFI_CAM_CONNECTED, /* IP assigned; driver probe pending                        */
    WIFI_CAM_PROBING,
    WIFI_CAM_READY,     /* Camera confirmed ready for recording commands            */
} wifi_cam_status_t;

/* ---- Recording status (§7.4) ---- */
typedef enum {
    CAMERA_RECORDING_UNKNOWN = 0,
    CAMERA_RECORDING_IDLE,
    CAMERA_RECORDING_ACTIVE,
} camera_recording_status_t;

/* ---- Desired recording intent (§7.5) ---- */
typedef enum {
    DESIRED_RECORDING_UNKNOWN = 0,  /* No intent yet — mismatch correction suppressed */
    DESIRED_RECORDING_STOP    = 1,
    DESIRED_RECORDING_START   = 2,
} desired_recording_t;

/* ---- Mismatch correction action (§23.1) ---- */
typedef enum {
    MISMATCH_ACTION_NONE  = 0,
    MISMATCH_ACTION_START = 1,
    MISMATCH_ACTION_STOP  = 2,
} mismatch_action_t;

/* ---- Pair-attempt transport (which add-camera flow is in flight) ---- *
 *
 * The pair_attempt state machine is shared between the BLE add-camera flow
 * (handler_pair → open_gopro_ble) and the WiFi RC add-camera flow
 * (handler_rc_add → gopro_wifi_rc_add_camera).  The state names map cleanly
 * to both; the only transport-specific behavior is the cancel cleanup path
 * (BLE: ble_gap_terminate; RC: remove the registered slot).
 */
typedef enum {
    PAIR_TRANSPORT_BLE     = 0,   /* default — BLE add-camera flow */
    PAIR_TRANSPORT_WIFI_RC = 1,   /* WiFi RC-emulation add-camera flow */
} pair_attempt_transport_t;

/* ---- Pair-attempt state machine (add-camera flow) ---- */
typedef enum {
    PAIR_ATTEMPT_IDLE = 0,        /* No attempt has been started */
    PAIR_ATTEMPT_CONNECTING,      /* BLE L2 connect in flight, or RC probes sent */
    PAIR_ATTEMPT_BONDING,         /* BLE only: L2 up, waiting for encrypted bond */
    PAIR_ATTEMPT_PROVISIONING,    /* Slot registered, readiness sequence running */
    PAIR_ATTEMPT_SUCCESS,         /* Slot persisted; camera ready */
    PAIR_ATTEMPT_FAILED,          /* error_code + error_message valid */
} pair_attempt_state_t;

typedef enum {
    PAIR_ERROR_NONE = 0,
    PAIR_ERROR_SLOTS_FULL,         /* CAMERA_MAX_SLOTS reached */
    PAIR_ERROR_BLE_CONNECT_FAILED, /* ble_gap_connect failed / timed out */
    PAIR_ERROR_BOND_FAILED,        /* encryption / bond rejected */
    PAIR_ERROR_HWINFO_TIMEOUT,     /* GetHardwareInfo retry budget exhausted */
    PAIR_ERROR_MODEL_UNSUPPORTED,  /* Frozen / unsupported model */
    PAIR_ERROR_HANDSHAKE_TIMEOUT,  /* SetThirdPartyClient/SetCameraControlStatus failed */
    PAIR_ERROR_DISCONNECTED,       /* BLE dropped before reaching READY */
    PAIR_ERROR_CANCELLED,          /* POST /api/pair/cancel */
    PAIR_ERROR_INTERNAL,           /* Catch-all */
} pair_attempt_error_t;

/*
 * Pure mismatch step function — no side effects, no platform dependencies.
 * Exported here so mismatch.c can be compiled for host-side unit tests
 * without pulling in ESP-IDF headers (§23.2).
 */
mismatch_action_t mismatch_step(desired_recording_t       desired,
                                 camera_recording_status_t actual,
                                 bool                      grace_period_active);
