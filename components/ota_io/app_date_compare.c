/*
 * app_date_compare.c — Parse and compare __DATE__/__TIME__ build stamps.
 *
 * Replaces the previous strncmp on "MMM DD YYYY" strings in
 * boot_helpers.c:app_desc_is_newer.  Lexical sort of three-letter month
 * abbreviations does not agree with calendar order — e.g. "Nov" > "Jan"
 * lexically but Nov 2024 is older than Jan 2025.  That bug caused
 * recovery's /api/ota/boot-main to pick the older of two valid OTA slots
 * whenever the build months crossed an alphabetical-vs-calendar boundary.
 *
 * Approach: parse each string into a sortable integer.  Pure logic, no
 * ESP-IDF dependencies; host-testable (§23.2, mirrors
 * cam_core/reorder_validate.c).
 */
#include "app_date_compare.h"

#include <stdint.h>
#include <stddef.h>

/*
 * Parse a "MMM DD YYYY" string into a sortable YYYYMMDD integer.
 * Returns 0 on any parse failure so the caller can treat it as "unknown".
 *
 * __DATE__ format guarantees from C99 6.10.8:
 *   - chars 0..2  three-letter month abbreviation, first letter capital
 *   - char  3     space
 *   - chars 4..5  day, leading SPACE for days 1-9 (i.e. " 1" not "01")
 *   - char  6     space
 *   - chars 7..10 four-digit year
 */
static uint32_t parse_date(const char *s)
{
    static const char k_months[12][3] = {
        {'J','a','n'}, {'F','e','b'}, {'M','a','r'},
        {'A','p','r'}, {'M','a','y'}, {'J','u','n'},
        {'J','u','l'}, {'A','u','g'}, {'S','e','p'},
        {'O','c','t'}, {'N','o','v'}, {'D','e','c'},
    };

    if (!s) return 0;

    int m = 0;
    for (int i = 0; i < 12; i++) {
        if (s[0] == k_months[i][0] &&
            s[1] == k_months[i][1] &&
            s[2] == k_months[i][2]) {
            m = i + 1;
            break;
        }
    }
    if (m == 0)     return 0;
    if (s[3] != ' ') return 0;

    int d;
    if (s[4] == ' ') {
        if (s[5] < '0' || s[5] > '9') return 0;
        d = s[5] - '0';
    } else {
        if (s[4] < '0' || s[4] > '9' || s[5] < '0' || s[5] > '9') return 0;
        d = (s[4] - '0') * 10 + (s[5] - '0');
    }
    if (d < 1 || d > 31) return 0;

    if (s[6] != ' ') return 0;

    for (int i = 7; i <= 10; i++) {
        if (s[i] < '0' || s[i] > '9') return 0;
    }
    int y = (s[7]-'0')*1000 + (s[8]-'0')*100 + (s[9]-'0')*10 + (s[10]-'0');
    if (y < 1970 || y > 9999) return 0;

    return (uint32_t)y * 10000u + (uint32_t)m * 100u + (uint32_t)d;
}

/*
 * Parse a "hh:mm:ss" string into seconds-since-midnight (0..86399).
 * Returns -1 on parse failure.  Sentinel chosen so any legitimate
 * value compares non-equal to the failure result.
 */
static int32_t parse_time(const char *s)
{
    if (!s) return -1;
    for (int i = 0; i < 8; i++) {
        if (i == 2 || i == 5) {
            if (s[i] != ':') return -1;
        } else {
            if (s[i] < '0' || s[i] > '9') return -1;
        }
    }
    int h = (s[0]-'0')*10 + (s[1]-'0');
    int m = (s[3]-'0')*10 + (s[4]-'0');
    int sec = (s[6]-'0')*10 + (s[7]-'0');
    if (h > 23 || m > 59 || sec > 59) return -1;
    return h * 3600 + m * 60 + sec;
}

int app_date_time_compare(const char *date_a, const char *time_a,
                          const char *date_b, const char *time_b)
{
    uint32_t da = parse_date(date_a);
    uint32_t db = parse_date(date_b);

    /* If either date is unparseable, claim "equal" — refusing to pick a
     * winner is safer than guessing based on partial data. */
    if (da == 0 || db == 0) return 0;
    if (da != db) return da > db ? 1 : -1;

    int32_t ta = parse_time(time_a);
    int32_t tb = parse_time(time_b);
    if (ta < 0 || tb < 0) return 0;
    if (ta != tb) return ta > tb ? 1 : -1;
    return 0;
}
