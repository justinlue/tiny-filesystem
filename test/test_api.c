#include <string.h>
#include "check.h"
#include "sim_flash.h"

static struct sim_flash sim;
static struct tfs_config cfg;
static tfs_t fs;

static void setup(uint32_t sector_size, uint32_t sectors, uint32_t prog)
{
    sim_init(&sim, sector_size, sectors, prog);
    sim_config(&sim, &cfg);
}

static void teardown(void)
{
    sim_free(&sim);
}

/* Read the whole of `name` into buf; returns the byte count or an error. */
static int slurp(const char *name, char *buf, uint32_t cap)
{
    return tfs_read(&fs, name, 0, buf, cap);
}

static void blank_flash_does_not_mount(void)
{
    setup(1024, 4, 4);
    CHECK_EQ(tfs_mount(&fs, &cfg), TFS_ERR_CORRUPT);
    teardown();
}

static void written_file_reads_back_after_remount(void)
{
    char buf[64];
    setup(1024, 4, 4);
    CHECK_EQ(tfs_format(&fs, &cfg), 0);
    CHECK_EQ(tfs_mount(&fs, &cfg), 0);
    CHECK_EQ(tfs_write(&fs, "hello", "world", 5), 0);
    CHECK_EQ(tfs_mount(&fs, &cfg), 0);
    CHECK_EQ(tfs_size(&fs, "hello"), 5);
    CHECK_EQ(slurp("hello", buf, sizeof buf), 5);
    CHECK(memcmp(buf, "world", 5) == 0);
    CHECK_EQ(sim.violations, 0);
    teardown();
}

void api_tests(void)
{
    RUN(blank_flash_does_not_mount);
    RUN(written_file_reads_back_after_remount);
}
