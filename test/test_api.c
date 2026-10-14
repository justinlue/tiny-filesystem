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

static void rewrite_replaces_and_remove_deletes(void)
{
    char buf[64];
    setup(1024, 4, 4);
    CHECK_EQ(tfs_format(&fs, &cfg), 0);
    CHECK_EQ(tfs_mount(&fs, &cfg), 0);
    CHECK_EQ(tfs_write(&fs, "cfg", "first version", 13), 0);
    CHECK_EQ(tfs_write(&fs, "other", "x", 1), 0);
    CHECK_EQ(tfs_write(&fs, "cfg", "v2", 2), 0);
    CHECK_EQ(slurp("cfg", buf, sizeof buf), 2);
    CHECK(memcmp(buf, "v2", 2) == 0);
    CHECK_EQ(tfs_remove(&fs, "cfg"), 0);
    CHECK_EQ(tfs_size(&fs, "cfg"), TFS_ERR_NOENT);
    CHECK_EQ(slurp("cfg", buf, sizeof buf), TFS_ERR_NOENT);
    CHECK_EQ(tfs_remove(&fs, "cfg"), TFS_ERR_NOENT);
    CHECK_EQ(tfs_mount(&fs, &cfg), 0);
    CHECK_EQ(tfs_size(&fs, "cfg"), TFS_ERR_NOENT);
    CHECK_EQ(tfs_size(&fs, "other"), 1);
    CHECK_EQ(tfs_write(&fs, "cfg", "v3", 2), 0);   /* recreate after delete */
    CHECK_EQ(slurp("cfg", buf, sizeof buf), 2);
    CHECK(memcmp(buf, "v3", 2) == 0);
    CHECK_EQ(sim.violations, 0);
    teardown();
}

static void mounted(uint32_t sector_size, uint32_t sectors, uint32_t prog)
{
    setup(sector_size, sectors, prog);
    tfs_format(&fs, &cfg);
    tfs_mount(&fs, &cfg);
}

static void names_are_validated(void)
{
    mounted(1024, 4, 4);
    CHECK_EQ(tfs_write(&fs, "", "x", 1), TFS_ERR_INVAL);
    CHECK_EQ(tfs_write(&fs, 0, "x", 1), TFS_ERR_INVAL);
    CHECK_EQ(tfs_write(&fs, "0123456789abcdef", "x", 1), TFS_ERR_NAMETOOLONG);
    CHECK_EQ(tfs_size(&fs, "0123456789abcdef"), TFS_ERR_NAMETOOLONG);
    CHECK_EQ(tfs_write(&fs, "0123456789abcde", "x", 1), 0);   /* 15 bytes is fine */
    CHECK_EQ(tfs_write(&fs, "a/b", "1", 1), 0);               /* '/' is just a byte */
    CHECK_EQ(tfs_write(&fs, "A/b", "22", 2), 0);              /* case-sensitive */
    CHECK_EQ(tfs_size(&fs, "a/b"), 1);
    CHECK_EQ(tfs_size(&fs, "A/b"), 2);
    CHECK_EQ(tfs_size(&fs, "a"), TFS_ERR_NOENT);              /* no prefix matches */
    CHECK_EQ(sim.violations, 0);
    teardown();
}

static void empty_files_and_offset_reads(void)
{
    char buf[8];
    mounted(1024, 4, 4);
    CHECK_EQ(tfs_write(&fs, "empty", 0, 0), 0);
    CHECK_EQ(tfs_size(&fs, "empty"), 0);
    CHECK_EQ(tfs_read(&fs, "empty", 0, buf, sizeof buf), 0);
    CHECK_EQ(tfs_write(&fs, "f", "0123456789", 10), 0);
    CHECK_EQ(tfs_read(&fs, "f", 3, buf, 4), 4);
    CHECK(memcmp(buf, "3456", 4) == 0);
    CHECK_EQ(tfs_read(&fs, "f", 8, buf, 8), 2);
    CHECK(memcmp(buf, "89", 2) == 0);
    CHECK_EQ(tfs_read(&fs, "f", 10, buf, 8), 0);
    CHECK_EQ(tfs_read(&fs, "f", 99, buf, 8), 0);
    CHECK_EQ(tfs_write(&fs, "f", 0, 5), TFS_ERR_INVAL);
    teardown();
}

static void oversized_file_is_rejected_before_writing(void)
{
    static char big[1024];
    int max;
    mounted(1024, 4, 4);
    max = tfs_max_file_size(&cfg);
    /* 1024 - header 24 - reserved GC_DONE 20 - commit 4 - record header 16 - name 15 */
    CHECK_EQ(max, 945);
    CHECK_EQ(tfs_write(&fs, "big", big, (uint32_t)max + 1), TFS_ERR_FBIG);
    CHECK_EQ(tfs_size(&fs, "big"), TFS_ERR_NOENT);
    CHECK_EQ(tfs_write(&fs, "0123456789abcde", big, (uint32_t)max), 0);
    CHECK_EQ(tfs_size(&fs, "0123456789abcde"), max);
    CHECK_EQ(sim.violations, 0);
    teardown();
}

static void append_creates_then_extends(void)
{
    char buf[64];
    mounted(1024, 4, 4);
    CHECK_EQ(tfs_append(&fs, "log", "ab", 2), 0);
    CHECK_EQ(tfs_append(&fs, "log", "cde", 3), 0);
    CHECK_EQ(tfs_append(&fs, "log", "", 0), 0);
    CHECK_EQ(tfs_append(&fs, "log", "f", 1), 0);
    CHECK_EQ(tfs_size(&fs, "log"), 6);
    CHECK_EQ(slurp("log", buf, sizeof buf), 6);
    CHECK(memcmp(buf, "abcdef", 6) == 0);
    CHECK_EQ(tfs_read(&fs, "log", 1, buf, 4), 4);              /* spans three records */
    CHECK(memcmp(buf, "bcde", 4) == 0);
    CHECK_EQ(tfs_mount(&fs, &cfg), 0);
    CHECK_EQ(slurp("log", buf, sizeof buf), 6);
    CHECK(memcmp(buf, "abcdef", 6) == 0);
    CHECK_EQ(tfs_write(&fs, "log", "Z", 1), 0);                /* replace drops the chain */
    CHECK_EQ(tfs_append(&fs, "log", "Y", 1), 0);
    CHECK_EQ(slurp("log", buf, sizeof buf), 2);
    CHECK(memcmp(buf, "ZY", 2) == 0);
    CHECK_EQ(tfs_remove(&fs, "log"), 0);
    CHECK_EQ(tfs_append(&fs, "log", "new", 3), 0);             /* after delete: fresh file */
    CHECK_EQ(slurp("log", buf, sizeof buf), 3);
    CHECK(memcmp(buf, "new", 3) == 0);
    CHECK_EQ(sim.violations, 0);
    teardown();
}

static void long_append_chains_read_correctly(void)
{
    char buf[64], want[64];
    int i;
    mounted(1024, 4, 4);
    for (i = 0; i < 40; i++) {
        want[i] = (char)('A' + i % 26);
        CHECK_EQ(tfs_append(&fs, "log", &want[i], 1), 0);
        CHECK_EQ(tfs_size(&fs, "log"), i + 1);
    }
    CHECK_EQ(slurp("log", buf, sizeof buf), 40);
    CHECK(memcmp(buf, want, 40) == 0);
    CHECK_EQ(tfs_mount(&fs, &cfg), 0);
    CHECK_EQ(slurp("log", buf, sizeof buf), 40);
    CHECK(memcmp(buf, want, 40) == 0);
    CHECK_EQ(sim.violations, 0);
    teardown();
}

static void append_past_max_size_is_rejected(void)
{
    static char big[1024];
    mounted(1024, 4, 4);
    CHECK_EQ(tfs_write(&fs, "f", big, 940), 0);
    CHECK_EQ(tfs_append(&fs, "f", big, 6), TFS_ERR_FBIG);
    CHECK_EQ(tfs_append(&fs, "f", big, 5), 0);
    CHECK_EQ(tfs_size(&fs, "f"), 945);
    teardown();
}

void api_tests(void)
{
    RUN(blank_flash_does_not_mount);
    RUN(written_file_reads_back_after_remount);
    RUN(rewrite_replaces_and_remove_deletes);
    RUN(names_are_validated);
    RUN(empty_files_and_offset_reads);
    RUN(oversized_file_is_rejected_before_writing);
    RUN(append_creates_then_extends);
    RUN(long_append_chains_read_correctly);
    RUN(append_past_max_size_is_rejected);
}
