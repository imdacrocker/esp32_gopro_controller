/*
 * mismatch.c — Pure mismatch correction step function.
 *
 * Intentionally free of ESP-IDF includes so this file can be compiled and
 * tested on the host without the full IDF toolchain (§23.2).
 *
 * Truth table (§13.4): 3 × 3 × 2 = 18 combinations.
 * Both DESIRED_UNKNOWN and RECORDING_UNKNOWN suppress correction.
 * An active grace period also suppresses (command already in flight).
 */
#include "camera_types.h"

mismatch_action_t mismatch_step(desired_recording_t       desired,
                                 camera_recording_status_t actual,
                                 bool                      grace_period_active)
{
    if (desired == DESIRED_RECORDING_UNKNOWN)  return MISMATCH_ACTION_NONE;
    if (actual  == CAMERA_RECORDING_UNKNOWN)   return MISMATCH_ACTION_NONE;
    if (grace_period_active)                   return MISMATCH_ACTION_NONE;

    if (desired == DESIRED_RECORDING_START && actual == CAMERA_RECORDING_IDLE)
        return MISMATCH_ACTION_START;

    if (desired == DESIRED_RECORDING_STOP && actual == CAMERA_RECORDING_ACTIVE)
        return MISMATCH_ACTION_STOP;

    return MISMATCH_ACTION_NONE;
}
