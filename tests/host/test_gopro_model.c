/*
 * Host-side unit tests for the gopro_model capability helpers and the
 * model_name lookup table (docs/design/camera-manager.md §5.2, §23.1).
 */
#include <stdio.h>
#include "unity.h"
#include "camera_types.h"
#include "gopro_model.h"

void setUp(void) {}
void tearDown(void) {}

/* Every real (non-UNKNOWN) GoPro model the firmware knows about. */
static const camera_model_t k_all_models[] = {
    CAMERA_MODEL_GOPRO_HERO2,
    CAMERA_MODEL_GOPRO_HERO3_WHITE,
    CAMERA_MODEL_GOPRO_HERO3_SILVER,
    CAMERA_MODEL_GOPRO_HERO3_BLACK,
    CAMERA_MODEL_GOPRO_HERO3PLUS_SILVER,
    CAMERA_MODEL_GOPRO_HERO3PLUS_BLACK,
    CAMERA_MODEL_GOPRO_HEROPLUS_LCD,
    CAMERA_MODEL_GOPRO_HEROPLUS,
    CAMERA_MODEL_GOPRO_HERO4_SILVER,
    CAMERA_MODEL_GOPRO_HERO4_BLACK,
    CAMERA_MODEL_GOPRO_HERO4_SESSION,
    CAMERA_MODEL_GOPRO_HERO5_BLACK,
    CAMERA_MODEL_GOPRO_HERO5_SESSION,
    CAMERA_MODEL_GOPRO_HERO6_BLACK,
    CAMERA_MODEL_GOPRO_HERO_2018,
    CAMERA_MODEL_GOPRO_HERO_LEGACY_RC,
    CAMERA_MODEL_GOPRO_HERO7_BLACK,
    CAMERA_MODEL_GOPRO_HERO8_BLACK,
    CAMERA_MODEL_GOPRO_HERO9_BLACK,
    CAMERA_MODEL_GOPRO_HERO10_BLACK,
    CAMERA_MODEL_GOPRO_HERO11_BLACK,
    CAMERA_MODEL_GOPRO_HERO11_MINI,
    CAMERA_MODEL_GOPRO_HERO12_BLACK,
    CAMERA_MODEL_GOPRO_MAX2,
    CAMERA_MODEL_GOPRO_HERO13_BLACK,
    CAMERA_MODEL_GOPRO_LIT_HERO,
};
#define N_MODELS (sizeof(k_all_models) / sizeof(k_all_models[0]))

/* ---- gopro_model_from_name ---- */

void test_from_name_known_models(void)
{
    TEST_ASSERT_EQUAL_INT(CAMERA_MODEL_GOPRO_HERO4_BLACK,  gopro_model_from_name("HERO4 Black"));
    TEST_ASSERT_EQUAL_INT(CAMERA_MODEL_GOPRO_HERO4_SILVER, gopro_model_from_name("HERO4 Silver"));
    TEST_ASSERT_EQUAL_INT(CAMERA_MODEL_GOPRO_HERO7_BLACK,  gopro_model_from_name("HERO7 Black"));
    TEST_ASSERT_EQUAL_INT(CAMERA_MODEL_GOPRO_HERO_2018,    gopro_model_from_name("HERO 2018"));
}

void test_from_name_null_is_unknown(void)
{
    TEST_ASSERT_EQUAL_INT(CAMERA_MODEL_UNKNOWN, gopro_model_from_name(NULL));
}

void test_from_name_unrecognised_falls_back_to_legacy_rc(void)
{
    TEST_ASSERT_EQUAL_INT(CAMERA_MODEL_GOPRO_HERO_LEGACY_RC, gopro_model_from_name("HERO99 Plaid"));
    TEST_ASSERT_EQUAL_INT(CAMERA_MODEL_GOPRO_HERO_LEGACY_RC, gopro_model_from_name(""));
}

void test_from_name_is_case_sensitive(void)
{
    /* The table documents exact-case matching; a lowercase variant must miss. */
    TEST_ASSERT_EQUAL_INT(CAMERA_MODEL_GOPRO_HERO_LEGACY_RC, gopro_model_from_name("hero4 black"));
}

/* ---- Capability helper invariants ---- */

void test_all_table_models_are_gopro(void)
{
    for (size_t i = 0; i < N_MODELS; i++) {
        TEST_ASSERT_TRUE_MESSAGE(gopro_model_is_gopro(k_all_models[i]),
                                 "model missing from gopro_model_is_gopro");
    }
}

void test_unknown_is_not_gopro(void)
{
    TEST_ASSERT_FALSE(gopro_model_is_gopro(CAMERA_MODEL_UNKNOWN));
    TEST_ASSERT_FALSE(gopro_model_uses_rc_emulation(CAMERA_MODEL_UNKNOWN));
    TEST_ASSERT_FALSE(gopro_model_uses_ble_control(CAMERA_MODEL_UNKNOWN));
}

/* Every known model must be controlled over exactly one transport. */
void test_rc_and_ble_are_mutually_exclusive_and_total(void)
{
    for (size_t i = 0; i < N_MODELS; i++) {
        bool rc  = gopro_model_uses_rc_emulation(k_all_models[i]);
        bool ble = gopro_model_uses_ble_control(k_all_models[i]);
        char msg[64];
        snprintf(msg, sizeof(msg), "model=%d rc=%d ble=%d", k_all_models[i], rc, ble);
        TEST_ASSERT_TRUE_MESSAGE(rc != ble, msg);  /* exactly one transport */
    }
}

void test_hero6_7_8_routed_over_ble_not_rc(void)
{
    /* §5.2: Hero6/7/8 accept RC pairing but are deliberately driven via BLE. */
    const camera_model_t dual[] = {
        CAMERA_MODEL_GOPRO_HERO6_BLACK,
        CAMERA_MODEL_GOPRO_HERO7_BLACK,
        CAMERA_MODEL_GOPRO_HERO8_BLACK,
    };
    for (size_t i = 0; i < sizeof(dual) / sizeof(dual[0]); i++) {
        TEST_ASSERT_FALSE(gopro_model_uses_rc_emulation(dual[i]));
        TEST_ASSERT_TRUE(gopro_model_uses_ble_control(dual[i]));
    }
}

void test_udp_keepalive_and_manual_id_track_rc_emulation(void)
{
    for (size_t i = 0; i < N_MODELS; i++) {
        bool rc = gopro_model_uses_rc_emulation(k_all_models[i]);
        TEST_ASSERT_EQUAL_INT(rc, gopro_model_uses_udp_keepalive(k_all_models[i]));
        TEST_ASSERT_EQUAL_INT(rc, gopro_model_requires_manual_id(k_all_models[i]));
    }
}

void test_http_datetime_only_hero4_black_silver(void)
{
    for (size_t i = 0; i < N_MODELS; i++) {
        camera_model_t m = k_all_models[i];
        bool want = (m == CAMERA_MODEL_GOPRO_HERO4_BLACK ||
                     m == CAMERA_MODEL_GOPRO_HERO4_SILVER);
        TEST_ASSERT_EQUAL_INT(want, gopro_model_supports_http_datetime(m));
    }
}

void test_wifi_pair_complete_is_hero6_7_8(void)
{
    for (size_t i = 0; i < N_MODELS; i++) {
        camera_model_t m = k_all_models[i];
        bool want = (m == CAMERA_MODEL_GOPRO_HERO6_BLACK ||
                     m == CAMERA_MODEL_GOPRO_HERO7_BLACK ||
                     m == CAMERA_MODEL_GOPRO_HERO8_BLACK);
        TEST_ASSERT_EQUAL_INT(want, gopro_model_needs_wifi_pair_complete(m));
    }
}

void test_no_model_is_frozen(void)
{
    for (size_t i = 0; i < N_MODELS; i++) {
        TEST_ASSERT_FALSE(gopro_model_is_frozen(k_all_models[i]));
    }
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_from_name_known_models);
    RUN_TEST(test_from_name_null_is_unknown);
    RUN_TEST(test_from_name_unrecognised_falls_back_to_legacy_rc);
    RUN_TEST(test_from_name_is_case_sensitive);
    RUN_TEST(test_all_table_models_are_gopro);
    RUN_TEST(test_unknown_is_not_gopro);
    RUN_TEST(test_rc_and_ble_are_mutually_exclusive_and_total);
    RUN_TEST(test_hero6_7_8_routed_over_ble_not_rc);
    RUN_TEST(test_udp_keepalive_and_manual_id_track_rc_emulation);
    RUN_TEST(test_http_datetime_only_hero4_black_silver);
    RUN_TEST(test_wifi_pair_complete_is_hero6_7_8);
    RUN_TEST(test_no_model_is_frozen);
    return UNITY_END();
}
