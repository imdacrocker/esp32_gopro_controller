/*
 * camera_types.h — Pure C types shared by every product variant.  No
 * ESP-IDF / NimBLE headers included here so this header can be compiled
 * by host-side unit-test targets (docs/design/camera-manager.md §23.2)
 * and by future variants whose transport stack differs from the wireless
 * app.
 *
 * Per the multi-variant restructure plan (§4), wireless-only types
 * (`cam_ble_status_t`, `wifi_cam_status_t`, `pair_attempt_*`) live next
 * to the wireless camera_manager component, not here.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

/* Maximum number of camera slots the manager will track.  Lives in this
 * pure-logic header so pure-logic compilation units (mismatch.c,
 * reorder_validate.c) and their host tests can use it without pulling
 * in ESP-IDF headers. */
#define CAMERA_MAX_SLOTS 4

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
    CAMERA_MODEL_GOPRO_HERO8_BLACK  = 50,
    CAMERA_MODEL_GOPRO_HERO9_BLACK  = 55,
    CAMERA_MODEL_GOPRO_HERO10_BLACK = 57,
    CAMERA_MODEL_GOPRO_HERO11_BLACK = 58,
    CAMERA_MODEL_GOPRO_HERO11_MINI  = 60,
    CAMERA_MODEL_GOPRO_HERO12_BLACK = 62,
    CAMERA_MODEL_GOPRO_MAX2         = 64,
    CAMERA_MODEL_GOPRO_HERO13_BLACK = 65,
    CAMERA_MODEL_GOPRO_LIT_HERO     = 70,

    /* Identification-only models — recognised so logs/UI show the real model
     * and the cv/info probe doesn't fall back to HERO_LEGACY_RC.  Model numbers
     * + codenames cross-checked against the hypoxic GoPro-Research camera
     * database.  IMPORTANT: these are deliberately NOT wired into any
     * gopro_model.h capability predicate (uses_rc_emulation / uses_ble_control /
     * uses_usb_control) — transport/control support is unverified on hardware.
     * They are added to gopro_model_is_gopro only.  Wire capabilities in a
     * follow-up once a unit is tested (HERO 2024 is the most likely USB-control
     * candidate: same Ambarella H22 as LIT_HERO). */
    CAMERA_MODEL_GOPRO_HERO_2014    = 14,  /* HD3.20,  codename "Bolina"          */
    CAMERA_MODEL_GOPRO_FUSION       = 22,  /* FS1.04,  codename "Superbank" (360) */
    CAMERA_MODEL_GOPRO_HERO7_WHITE  = 32,  /* H18.02,  codename "Boomer"          */
    CAMERA_MODEL_GOPRO_HERO7_SILVER = 33,  /* H18.03,  codename "Badger"          */
    CAMERA_MODEL_GOPRO_MAX          = 51,  /* H19.03,  codename "Coconuts" (360)  */
    CAMERA_MODEL_GOPRO_HERO_2024    = 66,  /* H24.03,  codename "Fraction"        */
    CAMERA_MODEL_GOPRO_MISSION1_PRO = 69,  /* H26.01,  codename "Sandbar"         */
    CAMERA_MODEL_GOPRO_MISSION1     = 71,  /* H26.02,  codename "Sandbar_Lite"    */

    /* Future manufacturer blocks: 1000+ */
} camera_model_t;

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

/*
 * Pure mismatch step function — no side effects, no platform dependencies.
 * Exported here so mismatch.c can be compiled for host-side unit tests
 * without pulling in ESP-IDF headers (§23.2).
 */
mismatch_action_t mismatch_step(desired_recording_t       desired,
                                 camera_recording_status_t actual,
                                 bool                      grace_period_active);

/* REVIEW[cam_core:A1]: reorder is a wireless-only concept (only caller is
 * camera_manager_reorder_slots), yet this lives in cam_core. Defensible to
 * keep pure-logic test files together; flagged to decide whether it should
 * move to the wireless component. */
/*
 * Pure reorder-permutation validator — returns true iff `order` is a valid
 * permutation of [0, domain_size): exactly `domain_size` entries, each
 * index in [0, domain_size) appearing exactly once.  `count` must equal
 * `domain_size` (truncation/extension are rejected).  Used by
 * camera_manager_reorder_slots to guard against malformed inputs that
 * would otherwise duplicate or drop cameras.  No side effects, no
 * platform dependencies (§23.2).
 */
bool reorder_is_valid_permutation(const int *order, int count, int domain_size);
