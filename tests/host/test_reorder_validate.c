/*
 * test_reorder_validate.c — Unity tests for reorder_is_valid_permutation.
 *
 * Mirrors the layout of test_mismatch.c and test_gopro_model.c.  The
 * predicate guards camera_manager_reorder_slots against the two
 * silent-data-loss paths the previous loose validation allowed:
 * truncation (count < s_slot_count) and duplicate indices.
 */
#include "unity.h"
#include "camera_types.h"

void setUp(void)    {}
void tearDown(void) {}

/* ---- accept: legitimate permutations ----------------------------------- */

static void test_identity_permutation(void)
{
    int order[] = {0, 1, 2};
    TEST_ASSERT_TRUE(reorder_is_valid_permutation(order, 3, 3));
}

static void test_two_element_swap(void)
{
    int order[] = {1, 0};
    TEST_ASSERT_TRUE(reorder_is_valid_permutation(order, 2, 2));
}

static void test_full_reverse_at_max(void)
{
    int order[CAMERA_MAX_SLOTS];
    for (int i = 0; i < CAMERA_MAX_SLOTS; i++) order[i] = CAMERA_MAX_SLOTS - 1 - i;
    TEST_ASSERT_TRUE(reorder_is_valid_permutation(order, CAMERA_MAX_SLOTS, CAMERA_MAX_SLOTS));
}

static void test_single_element(void)
{
    int order[] = {0};
    TEST_ASSERT_TRUE(reorder_is_valid_permutation(order, 1, 1));
}

/* ---- reject: truncation (count < domain_size) -------------------------- */

static void test_count_less_than_domain_rejected(void)
{
    /* The "shrink the list" case that would previously lose old slot 0 and
     * duplicate the tail. */
    int order[] = {2};
    TEST_ASSERT_FALSE(reorder_is_valid_permutation(order, 1, 3));
}

static void test_count_less_than_domain_two_of_three(void)
{
    int order[] = {0, 1};
    TEST_ASSERT_FALSE(reorder_is_valid_permutation(order, 2, 3));
}

/* ---- reject: extension (count > domain_size) --------------------------- */

static void test_count_greater_than_domain_rejected(void)
{
    int order[] = {0, 1, 2};
    TEST_ASSERT_FALSE(reorder_is_valid_permutation(order, 3, 2));
}

/* ---- reject: duplicates ------------------------------------------------ */

static void test_all_duplicates_rejected(void)
{
    int order[] = {0, 0, 0};
    TEST_ASSERT_FALSE(reorder_is_valid_permutation(order, 3, 3));
}

static void test_one_duplicate_rejected(void)
{
    int order[] = {0, 1, 1};
    TEST_ASSERT_FALSE(reorder_is_valid_permutation(order, 3, 3));
}

static void test_duplicate_at_start(void)
{
    int order[] = {2, 2, 0};
    TEST_ASSERT_FALSE(reorder_is_valid_permutation(order, 3, 3));
}

/* ---- reject: out-of-range indices -------------------------------------- */

static void test_index_too_high_rejected(void)
{
    int order[] = {0, 1, 5};
    TEST_ASSERT_FALSE(reorder_is_valid_permutation(order, 3, 3));
}

static void test_index_equal_to_domain_rejected(void)
{
    /* domain_size is exclusive upper bound */
    int order[] = {0, 1, 3};
    TEST_ASSERT_FALSE(reorder_is_valid_permutation(order, 3, 3));
}

static void test_negative_index_rejected(void)
{
    int order[] = {0, 1, -1};
    TEST_ASSERT_FALSE(reorder_is_valid_permutation(order, 3, 3));
}

/* ---- reject: degenerate inputs ----------------------------------------- */

static void test_null_pointer_rejected(void)
{
    TEST_ASSERT_FALSE(reorder_is_valid_permutation(NULL, 3, 3));
}

static void test_zero_count_rejected(void)
{
    int order[] = {0};
    TEST_ASSERT_FALSE(reorder_is_valid_permutation(order, 0, 0));
}

static void test_negative_count_rejected(void)
{
    int order[] = {0};
    TEST_ASSERT_FALSE(reorder_is_valid_permutation(order, -1, 3));
}

static void test_oversize_count_rejected(void)
{
    int order[CAMERA_MAX_SLOTS + 1] = { 0 };
    TEST_ASSERT_FALSE(reorder_is_valid_permutation(order,
                                                    CAMERA_MAX_SLOTS + 1,
                                                    CAMERA_MAX_SLOTS + 1));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_identity_permutation);
    RUN_TEST(test_two_element_swap);
    RUN_TEST(test_full_reverse_at_max);
    RUN_TEST(test_single_element);
    RUN_TEST(test_count_less_than_domain_rejected);
    RUN_TEST(test_count_less_than_domain_two_of_three);
    RUN_TEST(test_count_greater_than_domain_rejected);
    RUN_TEST(test_all_duplicates_rejected);
    RUN_TEST(test_one_duplicate_rejected);
    RUN_TEST(test_duplicate_at_start);
    RUN_TEST(test_index_too_high_rejected);
    RUN_TEST(test_index_equal_to_domain_rejected);
    RUN_TEST(test_negative_index_rejected);
    RUN_TEST(test_null_pointer_rejected);
    RUN_TEST(test_zero_count_rejected);
    RUN_TEST(test_negative_count_rejected);
    RUN_TEST(test_oversize_count_rejected);
    return UNITY_END();
}
