#include <string.h>
#include "check.h"
#include "sim_flash.h"

static const uint8_t ff4[4] = {0xFF, 0xFF, 0xFF, 0xFF};

static void sim_rejects_programming_a_unit_twice(void)
{
    struct sim_flash s;
    sim_init(&s, 256, 2, 4);
    uint8_t a[4] = {0x0F, 0xFF, 0xFF, 0xFF}, b[4] = {0x0E, 0xFF, 0xFF, 0xFF};
    CHECK_EQ(sim_prog(&s, SIM_BASE, a, 4), 0);
    CHECK_EQ(sim_prog(&s, SIM_BASE, b, 4), SIM_ERR_RULE);   /* only clears bits, still twice */
    CHECK_EQ(s.violations, 1);
    CHECK_EQ(sim_erase(&s, SIM_BASE), 0);
    CHECK_EQ(sim_prog(&s, SIM_BASE, b, 4), 0);
    uint8_t r[4];
    CHECK_EQ(sim_read(&s, SIM_BASE, r, 4), 0);
    CHECK(memcmp(r, b, 4) == 0);
    sim_free(&s);
}

static void sim_rejects_unaligned_and_out_of_range(void)
{
    struct sim_flash s;
    sim_init(&s, 256, 2, 4);
    CHECK_EQ(sim_prog(&s, SIM_BASE + 2, ff4, 4), SIM_ERR_RULE);
    CHECK_EQ(sim_prog(&s, SIM_BASE, ff4, 3), SIM_ERR_RULE);
    CHECK_EQ(sim_prog(&s, SIM_BASE + 512, ff4, 4), SIM_ERR_RULE);
    CHECK_EQ(sim_prog(&s, SIM_BASE - 4, ff4, 4), SIM_ERR_RULE);
    CHECK_EQ(sim_erase(&s, SIM_BASE + 128), SIM_ERR_RULE);
    CHECK_EQ(s.violations, 5);
    sim_free(&s);
}

static void cut_program_clears_only_a_subset_and_then_power_is_off(void)
{
    struct sim_flash s;
    int seed, partial = 0;
    for (seed = 1; seed <= 20; seed++) {
        uint8_t zero[8] = {0}, r[8];
        int i, any_cleared = 0, all_cleared = 1;
        sim_init(&s, 256, 2, 8);
        sim_cut_after(&s, 1, (uint32_t)seed);
        CHECK_EQ(sim_prog(&s, SIM_BASE, zero, 8), 0);          /* op 1 succeeds */
        CHECK_EQ(sim_prog(&s, SIM_BASE + 8, zero, 8), SIM_ERR_CUT);
        CHECK_EQ(sim_read(&s, SIM_BASE, r, 8), SIM_ERR_CUT);   /* power is off */
        sim_power_on(&s);
        CHECK_EQ(sim_read(&s, SIM_BASE + 8, r, 8), 0);
        for (i = 0; i < 8; i++) {
            any_cleared |= r[i] != 0xFF;
            all_cleared &= r[i] == 0;
        }
        partial += any_cleared && !all_cleared;
        CHECK_EQ(sim_prog(&s, SIM_BASE + 8, zero, 8), SIM_ERR_RULE);  /* unit was used */
        CHECK_EQ(sim_read(&s, SIM_BASE, r, 8), 0);
        CHECK(memcmp(r, zero, 8) == 0);
        sim_free(&s);
    }
    CHECK(partial > 10);
}

static void cut_erase_leaves_junk_that_cannot_be_programmed(void)
{
    struct sim_flash s;
    uint8_t zero[4] = {0}, r[4];
    uint32_t a;
    int junk = 0, erased = 0;
    sim_init(&s, 256, 2, 4);
    for (a = 0; a < 256; a += 4)
        CHECK_EQ(sim_prog(&s, SIM_BASE + a, zero, 4), 0);
    sim_cut_after(&s, 0, 7);
    CHECK_EQ(sim_erase(&s, SIM_BASE), SIM_ERR_CUT);
    sim_power_on(&s);
    for (a = 0; a < 256; a += 4) {
        CHECK_EQ(sim_read(&s, SIM_BASE + a, r, 4), 0);
        if (memcmp(r, ff4, 4) == 0) {
            erased++;
            CHECK_EQ(sim_prog(&s, SIM_BASE + a, zero, 4), 0);
        } else {
            junk++;
            CHECK_EQ(sim_prog(&s, SIM_BASE + a, zero, 4), SIM_ERR_RULE);
        }
    }
    CHECK(junk > 0 && erased > 0);
    sim_free(&s);
}

void sim_tests(void)
{
    RUN(sim_rejects_programming_a_unit_twice);
    RUN(sim_rejects_unaligned_and_out_of_range);
    RUN(cut_program_clears_only_a_subset_and_then_power_is_off);
    RUN(cut_erase_leaves_junk_that_cannot_be_programmed);
}
