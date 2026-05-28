/*
 * Host-side unit tests for mismatch_step() (docs/design/camera-manager.md §13.4).
 *
 * Exhaustively covers the 3 x 3 x 2 = 18-combination truth table: any UNKNOWN
 * input or an active grace period suppresses correction; otherwise a clear
 * desired-vs-actual disagreement yields the corresponding START/STOP action.
 */
#include <stdio.h>
#include "unity.h"
#include "camera_types.h"

void setUp(void) {}
void tearDown(void) {}

/* Independent re-statement of the §13.4 truth table, used as the oracle. */
static mismatch_action_t expected_action(desired_recording_t desired,
                                         camera_recording_status_t actual,
                                         bool grace)
{
    if (desired == DESIRED_RECORDING_UNKNOWN) return MISMATCH_ACTION_NONE;
    if (actual == CAMERA_RECORDING_UNKNOWN)   return MISMATCH_ACTION_NONE;
    if (grace)                                return MISMATCH_ACTION_NONE;
    if (desired == DESIRED_RECORDING_START && actual == CAMERA_RECORDING_IDLE)
        return MISMATCH_ACTION_START;
    if (desired == DESIRED_RECORDING_STOP && actual == CAMERA_RECORDING_ACTIVE)
        return MISMATCH_ACTION_STOP;
    return MISMATCH_ACTION_NONE;
}

void test_truth_table_exhaustive(void)
{
    const desired_recording_t desireds[] = {
        DESIRED_RECORDING_UNKNOWN, DESIRED_RECORDING_STOP, DESIRED_RECORDING_START };
    const camera_recording_status_t actuals[] = {
        CAMERA_RECORDING_UNKNOWN, CAMERA_RECORDING_IDLE, CAMERA_RECORDING_ACTIVE };

    for (int d = 0; d < 3; d++) {
        for (int a = 0; a < 3; a++) {
            for (int g = 0; g < 2; g++) {
                bool grace = (g == 1);
                mismatch_action_t got = mismatch_step(desireds[d], actuals[a], grace);
                mismatch_action_t want = expected_action(desireds[d], actuals[a], grace);
                char msg[96];
                snprintf(msg, sizeof(msg), "desired=%d actual=%d grace=%d",
                         desireds[d], actuals[a], grace);
                TEST_ASSERT_EQUAL_INT_MESSAGE(want, got, msg);
            }
        }
    }
}

/* Spot-check the two actionable corrections directly, independent of the oracle. */
void test_start_when_idle_but_should_record(void)
{
    TEST_ASSERT_EQUAL_INT(MISMATCH_ACTION_START,
        mismatch_step(DESIRED_RECORDING_START, CAMERA_RECORDING_IDLE, false));
}

void test_stop_when_active_but_should_idle(void)
{
    TEST_ASSERT_EQUAL_INT(MISMATCH_ACTION_STOP,
        mismatch_step(DESIRED_RECORDING_STOP, CAMERA_RECORDING_ACTIVE, false));
}

void test_grace_period_suppresses_correction(void)
{
    TEST_ASSERT_EQUAL_INT(MISMATCH_ACTION_NONE,
        mismatch_step(DESIRED_RECORDING_START, CAMERA_RECORDING_IDLE, true));
    TEST_ASSERT_EQUAL_INT(MISMATCH_ACTION_NONE,
        mismatch_step(DESIRED_RECORDING_STOP, CAMERA_RECORDING_ACTIVE, true));
}

void test_already_in_desired_state_is_noop(void)
{
    TEST_ASSERT_EQUAL_INT(MISMATCH_ACTION_NONE,
        mismatch_step(DESIRED_RECORDING_START, CAMERA_RECORDING_ACTIVE, false));
    TEST_ASSERT_EQUAL_INT(MISMATCH_ACTION_NONE,
        mismatch_step(DESIRED_RECORDING_STOP, CAMERA_RECORDING_IDLE, false));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_truth_table_exhaustive);
    RUN_TEST(test_start_when_idle_but_should_record);
    RUN_TEST(test_stop_when_active_but_should_idle);
    RUN_TEST(test_grace_period_suppresses_correction);
    RUN_TEST(test_already_in_desired_state_is_noop);
    return UNITY_END();
}
