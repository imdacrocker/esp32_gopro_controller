/*
 * app_date_compare.h — Pure date/time string comparator for esp_app_desc_t.
 *
 * Extracted so the parsing logic is host-testable (§23.2, same pattern as
 * cam_core/reorder_validate.c).  Used by ota_io/boot_helpers.c when
 * two OTA slots tie on secure_version and the function needs a tiebreaker.
 */
#pragma once

/*
 * Compare two pairs of (date, time) strings as produced by the C
 * preprocessor's __DATE__ / __TIME__ macros and baked into esp_app_desc_t:
 *
 *   date: "MMM DD YYYY" — 11 chars exact. Month is a 3-letter abbreviation
 *         ("Jan", "Feb", ..., "Dec"); DD has a leading space for single-
 *         digit days (per C99 6.10.8 / __DATE__).
 *   time: "hh:mm:ss"   — 8 chars exact, zero-padded.
 *
 * Returns:
 *    +1  if a is strictly newer than b
 *     0  if a and b are equal, OR if either string fails to parse
 *    -1  if a is strictly older than b
 *
 * Treating unparseable inputs as "equal" is deliberate — the only caller
 * (boot_helpers.c:app_desc_is_newer) uses the return value as a
 * tiebreaker after secure_version has already matched, so returning 0
 * means "don't claim either slot is newer," which is safer than guessing.
 *
 * Pure logic, no ESP-IDF / FreeRTOS dependencies; host-testable.
 */
int app_date_time_compare(const char *date_a, const char *time_a,
                          const char *date_b, const char *time_b);
