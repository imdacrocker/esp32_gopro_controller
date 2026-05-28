/*
 * test_app_date_compare.c — Unity tests for app_date_time_compare.
 *
 * Documents the lexical-sort bug that motivated extracting this helper:
 * the previous boot_helpers.c:app_desc_is_newer used strncmp on
 * "MMM DD YYYY" strings, which gives the wrong answer whenever two months'
 * alphabetical order disagrees with calendar order — most commonly when
 * an OTA crosses a year boundary (e.g. "Nov 30 2024" vs "Jan 15 2025").
 * Each test below names the bug class it guards against.
 */
#include "unity.h"
#include "app_date_compare.h"

void setUp(void)    {}
void tearDown(void) {}

/* ---- equality / no-change cases ---------------------------------------- */

static void test_identical_dates_and_times_equal(void)
{
    TEST_ASSERT_EQUAL_INT(0,
        app_date_time_compare("Jan 15 2025", "12:00:00",
                              "Jan 15 2025", "12:00:00"));
}

/* ---- year wins over everything ----------------------------------------- */

static void test_newer_year_wins(void)
{
    /* Dec 31 2024 vs Jan 01 2025 — Dec > Jan lexically (D < J actually, so
     * lex agrees here), but the test is really "newer year wins regardless
     * of month string sort order." */
    TEST_ASSERT_EQUAL_INT(1,
        app_date_time_compare("Jan  1 2025", "00:00:00",
                              "Dec 31 2024", "23:59:59"));
    TEST_ASSERT_EQUAL_INT(-1,
        app_date_time_compare("Dec 31 2024", "23:59:59",
                              "Jan  1 2025", "00:00:00"));
}

/* ---- the smoking-gun bug: lex-sort regression across calendar order --- */

static void test_nov_vs_jan_cross_year(void)
{
    /* "Nov 30 2024" vs "Jan 15 2025": 'J' < 'N' lex, so the old strncmp
     * code reported Nov as newer; the calendar truth is Jan 2025 wins. */
    TEST_ASSERT_EQUAL_INT(1,
        app_date_time_compare("Jan 15 2025", "00:00:00",
                              "Nov 30 2024", "23:59:59"));
    TEST_ASSERT_EQUAL_INT(-1,
        app_date_time_compare("Nov 30 2024", "23:59:59",
                              "Jan 15 2025", "00:00:00"));
}

static void test_sep_vs_jan_cross_year(void)
{
    /* "Sep 28 2024" vs "Jan 10 2025": 'J' < 'S' lex, old code wrong. */
    TEST_ASSERT_EQUAL_INT(1,
        app_date_time_compare("Jan 10 2025", "00:00:00",
                              "Sep 28 2024", "00:00:00"));
}

static void test_oct_vs_apr_cross_year(void)
{
    /* "Oct 5 2024" vs "Apr 1 2025": 'A' < 'O' lex, so Oct sorts after Apr,
     * but Apr 2025 is calendar-newer. */
    TEST_ASSERT_EQUAL_INT(1,
        app_date_time_compare("Apr  1 2025", "00:00:00",
                              "Oct  5 2024", "23:00:00"));
}

/* ---- same-year alphabetical regressions -------------------------------- */

static void test_feb_vs_jan_same_year(void)
{
    /* 'F' < 'J' lex but Feb > Jan calendar.  Old code claimed Jan newer. */
    TEST_ASSERT_EQUAL_INT(1,
        app_date_time_compare("Feb  5 2025", "00:00:00",
                              "Jan 30 2025", "00:00:00"));
}

static void test_apr_vs_mar_same_year(void)
{
    /* 'A' < 'M' lex but Apr > Mar calendar. */
    TEST_ASSERT_EQUAL_INT(1,
        app_date_time_compare("Apr 15 2025", "00:00:00",
                              "Mar 30 2025", "00:00:00"));
}

static void test_dec_vs_feb_same_year(void)
{
    /* 'D' < 'F' lex but Dec > Feb calendar. */
    TEST_ASSERT_EQUAL_INT(1,
        app_date_time_compare("Dec 25 2025", "00:00:00",
                              "Feb  1 2025", "00:00:00"));
}

/* ---- day comparison (the __DATE__ leading-space format) --------------- */

static void test_single_digit_day_leading_space(void)
{
    /* __DATE__ uses a leading space for days 1-9: " 5" not "05".  Make sure
     * the parser handles both columns correctly and treats "15" > " 5". */
    TEST_ASSERT_EQUAL_INT(1,
        app_date_time_compare("Jan 15 2025", "00:00:00",
                              "Jan  5 2025", "00:00:00"));
}

static void test_two_digit_day_correctness(void)
{
    /* And "31" > "15" — both two-digit. */
    TEST_ASSERT_EQUAL_INT(1,
        app_date_time_compare("Jan 31 2025", "00:00:00",
                              "Jan 15 2025", "00:00:00"));
}

/* ---- time tiebreaker on identical dates -------------------------------- */

static void test_time_tiebreaker(void)
{
    TEST_ASSERT_EQUAL_INT(1,
        app_date_time_compare("Jan 15 2025", "14:30:00",
                              "Jan 15 2025", "09:00:00"));
    TEST_ASSERT_EQUAL_INT(-1,
        app_date_time_compare("Jan 15 2025", "09:00:00",
                              "Jan 15 2025", "14:30:00"));
}

static void test_time_tiebreaker_second_precision(void)
{
    TEST_ASSERT_EQUAL_INT(1,
        app_date_time_compare("Jan 15 2025", "12:00:01",
                              "Jan 15 2025", "12:00:00"));
}

/* ---- malformed input: refuse to pick a winner -------------------------- */

static void test_malformed_month_returns_equal(void)
{
    /* "Xxx" isn't a valid month; parser returns 0; comparator returns 0. */
    TEST_ASSERT_EQUAL_INT(0,
        app_date_time_compare("Xxx 15 2025", "12:00:00",
                              "Jan 15 2025", "12:00:00"));
}

static void test_malformed_day_returns_equal(void)
{
    TEST_ASSERT_EQUAL_INT(0,
        app_date_time_compare("Jan AB 2025", "12:00:00",
                              "Jan 15 2025", "12:00:00"));
}

static void test_malformed_year_returns_equal(void)
{
    TEST_ASSERT_EQUAL_INT(0,
        app_date_time_compare("Jan 15 20XX", "12:00:00",
                              "Jan 15 2025", "12:00:00"));
}

static void test_malformed_time_with_matching_dates_returns_equal(void)
{
    /* Dates match; broken time on one side; don't accidentally invert. */
    TEST_ASSERT_EQUAL_INT(0,
        app_date_time_compare("Jan 15 2025", "XX:00:00",
                              "Jan 15 2025", "12:00:00"));
}

static void test_null_inputs_return_equal(void)
{
    TEST_ASSERT_EQUAL_INT(0,
        app_date_time_compare(NULL, "12:00:00",
                              "Jan 15 2025", "12:00:00"));
    TEST_ASSERT_EQUAL_INT(0,
        app_date_time_compare("Jan 15 2025", NULL,
                              "Jan 15 2025", "12:00:00"));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_identical_dates_and_times_equal);
    RUN_TEST(test_newer_year_wins);
    RUN_TEST(test_nov_vs_jan_cross_year);
    RUN_TEST(test_sep_vs_jan_cross_year);
    RUN_TEST(test_oct_vs_apr_cross_year);
    RUN_TEST(test_feb_vs_jan_same_year);
    RUN_TEST(test_apr_vs_mar_same_year);
    RUN_TEST(test_dec_vs_feb_same_year);
    RUN_TEST(test_single_digit_day_leading_space);
    RUN_TEST(test_two_digit_day_correctness);
    RUN_TEST(test_time_tiebreaker);
    RUN_TEST(test_time_tiebreaker_second_precision);
    RUN_TEST(test_malformed_month_returns_equal);
    RUN_TEST(test_malformed_day_returns_equal);
    RUN_TEST(test_malformed_year_returns_equal);
    RUN_TEST(test_malformed_time_with_matching_dates_returns_equal);
    RUN_TEST(test_null_inputs_return_equal);
    return UNITY_END();
}
