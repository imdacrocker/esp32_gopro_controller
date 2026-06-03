/*
 * reorder_validate.c — Pure permutation-of-[0, N) validator for the slot
 * reorder API.
 *
 * Intentionally free of ESP-IDF includes so this file can be compiled and
 * tested on the host without the full IDF toolchain (§23.2, mirrors
 * mismatch.c).
 *
 * Why strict permutation: camera_manager_reorder_slots semantically means
 * "reorder my existing slots into this new arrangement."  Anything that
 * isn't a permutation of the current slots — wrong count, out-of-range
 * index, duplicates — would either silently drop a camera (truncation)
 * or silently duplicate one (repeated index), with corresponding NVS
 * corruption.  Reject up front; deletion has its own path via
 * camera_manager_remove_slot.
 */
#include "camera_types.h"

bool reorder_is_valid_permutation(const int *order, int count, int domain_size)
{
    if (!order) return false;
    if (count <= 0 || count > CAMERA_MAX_SLOTS) return false;
    if (domain_size <= 0 || domain_size > CAMERA_MAX_SLOTS) return false;
    if (count != domain_size) return false;

    bool seen[CAMERA_MAX_SLOTS] = { false };
    for (int i = 0; i < count; i++) {
        int v = order[i];
        if (v < 0 || v >= domain_size) return false;
        if (seen[v]) return false;
        seen[v] = true;
    }
    return true;
}
