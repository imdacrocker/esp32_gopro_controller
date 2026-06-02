/*
 * gopro_model.h — GoPro-specific camera_model_t capability helpers (§5.2).
 *
 * These helpers are called ONLY by gopro components.  camera_manager never
 * imports this header — all behavioral branching in higher-level components
 * goes through these inline predicates rather than comparing raw model values.
 *
 * Each helper enumerates every known model explicitly.  A new model ID must
 * be consciously added to each applicable helper — there is no silent auto-
 * inclusion based on a numeric range.
 *
 * Intentionally free of ESP-IDF headers so this file can be included by
 * host-side unit tests (§23.2).
 *
 * Hero4-class BLE control is not implemented.  Hero5 BLE-control is reportedly
 * functional but not enumerated here until verified on hardware.
 */
#pragma once

#include "camera_types.h"

/** True if the model is any known GoPro camera (RC-emulation or BLE-control). */
static inline bool gopro_model_is_gopro(camera_model_t model)
{
    return model == CAMERA_MODEL_GOPRO_HERO2
        || model == CAMERA_MODEL_GOPRO_HERO3_WHITE
        || model == CAMERA_MODEL_GOPRO_HERO3_SILVER
        || model == CAMERA_MODEL_GOPRO_HERO3_BLACK
        || model == CAMERA_MODEL_GOPRO_HERO3PLUS_SILVER
        || model == CAMERA_MODEL_GOPRO_HERO3PLUS_BLACK
        || model == CAMERA_MODEL_GOPRO_HEROPLUS_LCD
        || model == CAMERA_MODEL_GOPRO_HEROPLUS
        || model == CAMERA_MODEL_GOPRO_HERO4_SILVER
        || model == CAMERA_MODEL_GOPRO_HERO4_BLACK
        || model == CAMERA_MODEL_GOPRO_HERO4_SESSION
        || model == CAMERA_MODEL_GOPRO_HERO5_BLACK
        || model == CAMERA_MODEL_GOPRO_HERO5_SESSION
        || model == CAMERA_MODEL_GOPRO_HERO6_BLACK
        || model == CAMERA_MODEL_GOPRO_HERO_2018
        || model == CAMERA_MODEL_GOPRO_HERO_LEGACY_RC
        || model == CAMERA_MODEL_GOPRO_HERO7_BLACK
        || model == CAMERA_MODEL_GOPRO_HERO8_BLACK
        || model == CAMERA_MODEL_GOPRO_HERO9_BLACK
        || model == CAMERA_MODEL_GOPRO_HERO10_BLACK
        || model == CAMERA_MODEL_GOPRO_HERO11_BLACK
        || model == CAMERA_MODEL_GOPRO_HERO11_MINI
        || model == CAMERA_MODEL_GOPRO_HERO12_BLACK
        || model == CAMERA_MODEL_GOPRO_MAX2
        || model == CAMERA_MODEL_GOPRO_HERO13_BLACK
        || model == CAMERA_MODEL_GOPRO_LIT_HERO;
}

/**
 * Camera connects by emulating a GoPro WiFi Remote AP.
 *
 * Hero2 through Hero5 (and HERO 2018 / Hero4 Session) accept the original
 * Smart Remote pairing flow as a STA on our SoftAP.  Hero4 onwards in this
 * mode also reply to the binary UDP `cv` (camera version) opcode with a
 * length-prefixed firmware + model_name payload — that's how we identify the
 * specific model without HTTP.
 *
 * Hero6/Hero7/Hero8 also accept the Smart-Remote pairing flow, but we route
 * them via BLE (`uses_ble_control`) because BLE pairing supports full state
 * (start/stop/status) where RC-emulation only sends keepalive + shutter.
 * The dual-transport ambiguity is acknowledged but not disambiguated by a
 * persisted-transport field — listing a model in both helpers would cause
 * the RC driver to win on reload by main.c boot order, so we keep these
 * three out of `uses_rc_emulation`.
 */
static inline bool gopro_model_uses_rc_emulation(camera_model_t model)
{
    return model == CAMERA_MODEL_GOPRO_HERO2
        || model == CAMERA_MODEL_GOPRO_HERO3_WHITE
        || model == CAMERA_MODEL_GOPRO_HERO3_SILVER
        || model == CAMERA_MODEL_GOPRO_HERO3_BLACK
        || model == CAMERA_MODEL_GOPRO_HERO3PLUS_SILVER
        || model == CAMERA_MODEL_GOPRO_HERO3PLUS_BLACK
        || model == CAMERA_MODEL_GOPRO_HEROPLUS_LCD
        || model == CAMERA_MODEL_GOPRO_HEROPLUS
        || model == CAMERA_MODEL_GOPRO_HERO4_SILVER
        || model == CAMERA_MODEL_GOPRO_HERO4_BLACK
        || model == CAMERA_MODEL_GOPRO_HERO4_SESSION
        || model == CAMERA_MODEL_GOPRO_HERO5_BLACK
        || model == CAMERA_MODEL_GOPRO_HERO5_SESSION
        || model == CAMERA_MODEL_GOPRO_HERO_2018
        || model == CAMERA_MODEL_GOPRO_HERO_LEGACY_RC;
}

/**
 * Camera accepts the HTTP/1.0 `setup/date_time` command on its STA-interface
 * IP.  This is the only HTTP path we use today — identification is done over
 * UDP (`cv` opcode) for all cameras, so this predicate purely gates
 * `rc_send_datetime()`.
 *
 * Hero4 Black/Silver in Smart-Remote mode are confirmed working (Lua trace).
 * Hero3-class never had STA-side HTTP at all.  Add Hero4 Session / Hero5
 * once verified on hardware.
 */
static inline bool gopro_model_supports_http_datetime(camera_model_t model)
{
    return model == CAMERA_MODEL_GOPRO_HERO4_BLACK
        || model == CAMERA_MODEL_GOPRO_HERO4_SILVER;
}

/**
 * Map a `model_name` string from the camera's UDP `cv` response (or, on
 * Hero4-class, the gpControl JSON if we ever re-enable that path) to a
 * `camera_model_t` enum value.  Returns CAMERA_MODEL_GOPRO_HERO_LEGACY_RC for
 * an unrecognised name — slot still runs on the UDP-only path; the mapping
 * can be added once the actual reported string is observed (logged at INFO
 * by rc_parse_cv_response).  Returns CAMERA_MODEL_UNKNOWN only when name is
 * NULL.
 *
 * Implementation lives in `gopro_model.c` — string-compare table is too long
 * to keep inline.
 */
camera_model_t gopro_model_from_name(const char *model_name);

/** Camera is controlled over BLE (no WiFi association required). */
static inline bool gopro_model_uses_ble_control(camera_model_t model)
{
    return model == CAMERA_MODEL_GOPRO_HERO6_BLACK
        || model == CAMERA_MODEL_GOPRO_HERO7_BLACK
        || model == CAMERA_MODEL_GOPRO_HERO8_BLACK
        || model == CAMERA_MODEL_GOPRO_HERO9_BLACK
        || model == CAMERA_MODEL_GOPRO_HERO10_BLACK
        || model == CAMERA_MODEL_GOPRO_HERO11_BLACK
        || model == CAMERA_MODEL_GOPRO_HERO11_MINI
        || model == CAMERA_MODEL_GOPRO_HERO12_BLACK
        || model == CAMERA_MODEL_GOPRO_MAX2
        || model == CAMERA_MODEL_GOPRO_HERO13_BLACK
        || model == CAMERA_MODEL_GOPRO_LIT_HERO;
}

/** Keepalive must be sent over UDP; all other commands use HTTP. */
static inline bool gopro_model_uses_udp_keepalive(camera_model_t model)
{
    return gopro_model_uses_rc_emulation(model);
}

/**
 * Camera predates the OpenGoPro spec and uses the legacy BLE control protocol.
 *
 * Implications:
 *   - Initial mode is selected with SetMode (TLV cmd 0x02) rather than
 *     Load Preset Group (cmd 0x3E) used on newer cameras.
 *   - `SetCameraControlStatus` (Feature 0xF1 / Action 0x69) is skipped
 *     (legacy cameras return INVALID_PARAM).
 *
 * Hero5/Hero6/Hero8 are reasonable candidates for this list but stay out
 * until verified on hardware.
 */
static inline bool gopro_model_uses_legacy_ble(camera_model_t model)
{
    return model == CAMERA_MODEL_GOPRO_HERO7_BLACK;
}

/**
 * Camera requires the legacy WiFi pair-complete handshake before it will
 * register the controller in its paired-apps list.  Concretely: after BLE
 * SMP+GATT setup, the controller must connect to the camera's WiFi AP and
 * issue
 *     GET http://10.5.5.9/gp/gpControl/command/wireless/pair/complete
 *         ?success=1&deviceName=<CONFIG_DEVICE_IDENTITY_NAME>
 * Without this step the BLE bond is volatile — commands still work in real
 * time but the camera UI shows "no connection" and the bond is invalidated
 * on camera power-cycle.
 *
 * Verified on Hero7 (firmware HD7.01.01.90.71).  Hero6 and Hero8 are listed
 * on the assumption that they share the legacy BLE app-pairing model; remove
 * if hardware testing proves otherwise.  Hero5 may also need this — add once
 * verified.
 */
static inline bool gopro_model_needs_wifi_pair_complete(camera_model_t model)
{
    return model == CAMERA_MODEL_GOPRO_HERO6_BLACK
        || model == CAMERA_MODEL_GOPRO_HERO7_BLACK
        || model == CAMERA_MODEL_GOPRO_HERO8_BLACK;
}

/**
 * Camera is recognised but support is intentionally frozen — when one is
 * detected after pairing, the BLE connection is dropped and the slot is
 * removed.
 *
 * No models are currently frozen.  Predicate kept for future use.
 */
static inline bool gopro_model_is_frozen(camera_model_t model)
{
    (void)model;
    return false;
}

/**
 * Model cannot be read from the camera — must be selected by the user at
 * pairing time (web UI pairing flow for RC-emulation cameras).
 */
static inline bool gopro_model_requires_manual_id(camera_model_t model)
{
    return gopro_model_uses_rc_emulation(model);
}
