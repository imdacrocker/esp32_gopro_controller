/*
 * gopro_model.c — Implementation of the non-inline helpers from gopro_model.h.
 *
 * Currently just `gopro_model_from_name()`: a string-compare lookup mapping
 * GoPro `info.model_name` JSON values to camera_model_t enum values.  Used by
 * the gopro_wifi_rc HTTP identify probe (§17.2.5 of docs/design/camera-manager.md).
 *
 * Naming convention: the strings used in the table are the exact values
 * emitted by the camera's GET /gp/gpControl JSON `info.model_name` field —
 * verified against the gpControl-*.json reference dumps in goprowifihack.
 *
 * This file is the lone SRC of the shared `gopro_model` component
 * (components/gopro_model/); the wireless gopro drivers and the wired
 * `gopro_usb` driver reach it via `REQUIRES gopro_model`.
 */

#include <string.h>
#include "gopro_model.h"

/*
 * model_name → camera_model_t.  Strings are case-sensitive and must match the
 * exact `info.model_name` value emitted by the camera's gpControl JSON.
 *
 * Hero4 Black / Silver are verified against goprowifihack/HERO4/gpControl-*.
 * The remaining strings are taken from goprowifihack/CameraCodenames.md and
 * the matching gpControl-*.json reference dumps; if the actual camera reports
 * a different string the lookup falls through to LEGACY_RC and the
 * unrecognised model_name + model_number is logged at INFO so this table can
 * be corrected.  Wrong strings are therefore harmless.
 *
 * Hero3-class cameras (HERO2 through HERO+) are intentionally absent: they
 * have no HTTP server on the STA interface, so this lookup is never reached
 * for them.
 */
static const struct {
    const char    *name;
    camera_model_t model;
} k_model_table[] = {
    { "HERO4 Black",   CAMERA_MODEL_GOPRO_HERO4_BLACK   },
    { "HERO4 Silver",  CAMERA_MODEL_GOPRO_HERO4_SILVER  },
    { "HERO4 Session", CAMERA_MODEL_GOPRO_HERO4_SESSION },
    { "HERO5 Black",   CAMERA_MODEL_GOPRO_HERO5_BLACK   },
    { "HERO5 Session", CAMERA_MODEL_GOPRO_HERO5_SESSION },
    { "HERO6 Black",   CAMERA_MODEL_GOPRO_HERO6_BLACK   },
    /* "HERO7 Black" verified against a real Hero7 (firmware HD7.01.01.90.00):
     * UDP `cv` reply byte 32 onwards = 0x0b "HERO7 Black". */
    { "HERO7 Black",   CAMERA_MODEL_GOPRO_HERO7_BLACK   },
    { "HERO8 Black",   CAMERA_MODEL_GOPRO_HERO8_BLACK   },
    { "HERO 2018",     CAMERA_MODEL_GOPRO_HERO_2018     },
};

camera_model_t gopro_model_from_name(const char *model_name)
{
    if (!model_name) return CAMERA_MODEL_UNKNOWN;

    for (size_t i = 0; i < sizeof(k_model_table) / sizeof(k_model_table[0]); i++) {
        if (strcmp(model_name, k_model_table[i].name) == 0) {
            return k_model_table[i].model;
        }
    }

    /* Camera responded to UDP `cv` but its model_name isn't in our table.
     * Falling back to the legacy enum is conservative — rc_parse_cv_response
     * already logged the model_name + firmware at INFO so the table can be
     * extended later. */
    return CAMERA_MODEL_GOPRO_HERO_LEGACY_RC;
}
