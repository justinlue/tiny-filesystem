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
    /* 1024 - header 24 - reserved GC_DONE 20 - reserved DELETE 20 - commit 4
     *      - record header 16 - name 15 */
    CHECK_EQ(max, 925);
    cfg.sector_size = 4096;
    CHECK_EQ(tfs_max_file_size(&cfg), 3997);   /* the figure DESIGN.md quotes */
    cfg.sector_size = 1024;
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
    CHECK_EQ(tfs_write(&fs, "f", big, 920), 0);
    CHECK_EQ(tfs_append(&fs, "f", big, 6), TFS_ERR_FBIG);
    CHECK_EQ(tfs_append(&fs, "f", big, 5), 0);
    CHECK_EQ(tfs_size(&fs, "f"), 925);
    teardown();
}

static void dir_lists_live_files_with_sizes(void)
{
    tfs_dir_t it;
    struct tfs_info info;
    int seen[3] = {0, 0, 0}, n = 0, res;
    mounted(1024, 4, 4);
    CHECK_EQ(tfs_dir_open(&fs, &it), 0);
    CHECK_EQ(tfs_dir_next(&fs, &it, &info), 0);              /* empty fs */
    CHECK_EQ(tfs_write(&fs, "alpha", "1", 1), 0);
    CHECK_EQ(tfs_write(&fs, "gone", "x", 1), 0);
    CHECK_EQ(tfs_write(&fs, "beta", "22", 2), 0);
    CHECK_EQ(tfs_append(&fs, "beta", "33", 2), 0);
    CHECK_EQ(tfs_write(&fs, "0123456789abcde", "", 0), 0);
    CHECK_EQ(tfs_remove(&fs, "gone"), 0);
    CHECK_EQ(tfs_dir_open(&fs, &it), 0);
    while ((res = tfs_dir_next(&fs, &it, &info)) == 1) {
        n++;
        if (strcmp(info.name, "alpha") == 0 && info.size == 1)
            seen[0]++;
        else if (strcmp(info.name, "beta") == 0 && info.size == 4)
            seen[1]++;
        else if (strcmp(info.name, "0123456789abcde") == 0 && info.size == 0)
            seen[2]++;
    }
    CHECK_EQ(res, 0);
    CHECK_EQ(n, 3);
    CHECK(seen[0] == 1 && seen[1] == 1 && seen[2] == 1);
    CHECK_EQ(tfs_dir_next(&fs, &it, &info), 0);              /* stays at end */
    teardown();
}

static void sixty_fifth_file_is_rejected(void)
{
    char name[8];
    int i;
    mounted(4096, 4, 4);
    for (i = 0; i < TFS_MAX_FILES; i++) {
        sprintf(name, "f%d", i);
        CHECK_EQ(tfs_write(&fs, name, "", 0), 0);
    }
    CHECK_EQ(tfs_write(&fs, "one-more", "", 0), TFS_ERR_NOFILES);
    CHECK_EQ(tfs_append(&fs, "one-more", "x", 1), TFS_ERR_NOFILES);
    CHECK_EQ(tfs_write(&fs, "f7", "still ok", 8), 0);        /* rewrite needs no new id */
    CHECK_EQ(tfs_remove(&fs, "f3"), 0);
    CHECK_EQ(tfs_write(&fs, "one-more", "x", 1), 0);
    CHECK_EQ(tfs_size(&fs, "one-more"), 1);
    teardown();
}

/* Many rewrites of a few files: the log wraps through every sector many times. */
static void churn(uint32_t sectors, uint32_t prog)
{
    char name[4] = "f0", buf[200], want[3][200];
    uint32_t lens[3] = {0, 0, 0};
    int i, k;
    mounted(512, sectors, prog);
    for (i = 0; i < 3000; i++) {
        k = i % 3;
        name[1] = (char)('0' + k);
        lens[k] = (uint32_t)(i * 7 % 60);
        memset(want[k], 'a' + i % 26, lens[k]);
        if (i % 5 == 4) {                        /* some appends too */
            CHECK_EQ(tfs_append(&fs, name, want[k], 3), 0);
            lens[k] = (uint32_t)tfs_size(&fs, name);
            CHECK(lens[k] < sizeof buf);
            CHECK_EQ(tfs_read(&fs, name, 0, want[k], lens[k]), (int)lens[k]);
        } else {
            CHECK_EQ(tfs_write(&fs, name, want[k], lens[k]), 0);
        }
        if (i % 97 == 0)
            CHECK_EQ(tfs_mount(&fs, &cfg), 0);
        for (k = 0; k < 3 && i >= 2; k++) {
            name[1] = (char)('0' + k);
            CHECK_EQ(tfs_read(&fs, name, 0, buf, sizeof buf), (int)lens[k]);
            CHECK(memcmp(buf, want[k], lens[k]) == 0);
        }
    }
    CHECK(sim.ops > 3000);
    CHECK_EQ(sim.violations, 0);
    teardown();
}

static void churn_two_sectors(void) { churn(2, 4); }
static void churn_eight_sectors(void) { churn(8, 4); }
static void churn_prog16(void) { churn(4, 16); }

static void full_fs_says_nospc_and_keeps_data(void)
{
    static char data[925];
    char name[8], buf[300];
    int i, n, res;
    long ops;
    mounted(1024, 4, 4);
    for (n = 0;; n++) {
        sprintf(name, "f%d", n);
        memset(data, 'a' + n % 26, 300);
        res = tfs_write(&fs, name, data, 300);
        if (res == TFS_ERR_NOSPC)
            break;
        CHECK_EQ(res, 0);
    }
    /* 960 usable bytes per sector hold two 324-byte records; 3 sectors hold data */
    CHECK_EQ(n, 6);
    CHECK_EQ(tfs_write(&fs, name, data, 300), TFS_ERR_NOSPC);
    ops = sim.ops;
    /* clearly over capacity: rejected before any program or erase */
    CHECK_EQ(tfs_write(&fs, "huge", data, sizeof data), TFS_ERR_NOSPC);
    CHECK_EQ(sim.ops, ops);
    CHECK_EQ(tfs_remove(&fs, "f1"), 0);               /* a full fs can always delete */
    CHECK_EQ(tfs_write(&fs, name, data, 300), 0);
    CHECK_EQ(tfs_mount(&fs, &cfg), 0);
    for (i = 0; i <= n; i++) {
        sprintf(name, "f%d", i);
        memset(data, 'a' + i % 26, 300);
        if (i == 1) {
            CHECK_EQ(tfs_size(&fs, name), TFS_ERR_NOENT);
            continue;
        }
        CHECK_EQ(tfs_read(&fs, name, 0, buf, sizeof buf), (int)sizeof buf);
        CHECK(memcmp(buf, data, sizeof buf) == 0);
    }
    CHECK_EQ(sim.violations, 0);
    teardown();
}

/* Replacing a file keeps the old version until the new one commits, so both must fit. */
static void largest_file_rewrites_need_three_sectors(void)
{
    static char big[1024], buf[1024];
    int max, i;
    mounted(1024, 2, 16);
    max = tfs_max_file_size(&cfg);
    memset(big, 'a', sizeof big);
    CHECK_EQ(tfs_write(&fs, "0123456789abcde", big, (uint32_t)max), 0);
    CHECK_EQ(tfs_write(&fs, "0123456789abcde", big, (uint32_t)max), TFS_ERR_NOSPC);
    CHECK_EQ(tfs_write(&fs, "x", "", 0), TFS_ERR_NOSPC);
    CHECK_EQ(tfs_read(&fs, "0123456789abcde", 0, buf, sizeof buf), max);
    CHECK(memcmp(buf, big, (size_t)max) == 0);
    CHECK_EQ(tfs_remove(&fs, "0123456789abcde"), 0);     /* a full fs can always delete */
    CHECK_EQ(tfs_write(&fs, "x", "", 0), 0);
    CHECK_EQ(sim.violations, 0);
    teardown();

    mounted(1024, 3, 16);
    for (i = 0; i < 20; i++) {
        memset(big, 'a' + i, sizeof big);
        CHECK_EQ(tfs_write(&fs, "0123456789abcde", big, (uint32_t)max), 0);
    }
    CHECK_EQ(tfs_read(&fs, "0123456789abcde", 0, buf, sizeof buf), max);
    CHECK(memcmp(buf, big, (size_t)max) == 0);
    CHECK_EQ(sim.violations, 0);
    teardown();
}

static void idle_gc_saves_the_next_write_from_collecting(void)
{
    static char data[300];
    long erases;
    int i;
    mounted(1024, 4, 4);
    CHECK_EQ(tfs_gc(&fs), 0);                        /* free sectors left: nothing to do */
    for (i = 0; i < 9; i++)                          /* rewrites leave dead records behind */
        CHECK_EQ(tfs_write(&fs, i % 2 ? "a" : "b", data, sizeof data), 0);
    CHECK_EQ(tfs_mount(&fs, &cfg), 0);               /* head now takes no more records */
    CHECK_EQ(tfs_gc(&fs), 1);
    erases = sim.erases;
    CHECK_EQ(tfs_write(&fs, "c", "x", 1), 0);
    CHECK_EQ(sim.erases, erases);
    CHECK_EQ(tfs_size(&fs, "a"), 300);
    CHECK_EQ(tfs_size(&fs, "b"), 300);
    CHECK_EQ(sim.violations, 0);
    teardown();
}

static void bad_configs_are_rejected(void)
{
    struct tfs_config bad;
    setup(1024, 4, 4);
    bad = cfg; bad.sector_count = 1;
    CHECK_EQ(tfs_format(&fs, &bad), TFS_ERR_INVAL);
    bad = cfg; bad.prog_size = 3;
    CHECK_EQ(tfs_format(&fs, &bad), TFS_ERR_INVAL);
    bad = cfg; bad.prog_size = 32;
    CHECK_EQ(tfs_mount(&fs, &bad), TFS_ERR_INVAL);
    bad = cfg; bad.sector_size = 1000;
    CHECK_EQ(tfs_format(&fs, &bad), TFS_ERR_INVAL);
    bad = cfg; bad.erase = 0;
    CHECK_EQ(tfs_format(&fs, &bad), TFS_ERR_INVAL);
    CHECK_EQ(tfs_max_file_size(&bad), TFS_ERR_INVAL);
    CHECK_EQ(sim.ops, 0);
    teardown();
}

static void geometry_mismatch_does_not_mount(void)
{
    struct tfs_config other;
    setup(1024, 4, 4);
    CHECK_EQ(tfs_format(&fs, &cfg), 0);
    other = cfg; other.sector_count = 2;
    CHECK_EQ(tfs_mount(&fs, &other), TFS_ERR_CORRUPT);
    other = cfg; other.prog_size = 8;
    CHECK_EQ(tfs_mount(&fs, &other), TFS_ERR_CORRUPT);
    CHECK_EQ(tfs_mount(&fs, &cfg), 0);
    teardown();
}

static void flipped_bit_in_committed_data_is_reported(void)
{
    uint32_t i;
    mounted(1024, 4, 4);
    CHECK_EQ(tfs_write(&fs, "f", "precious data", 13), 0);
    for (i = 0; i < 4096 - 13; i++)          /* corrupt the stored payload */
        if (memcmp(sim.mem + i, "precious data", 13) == 0)
            break;
    CHECK(i < 4096 - 13);
    sim.mem[i + 3] ^= 0x10;
    CHECK_EQ(tfs_mount(&fs, &cfg), TFS_ERR_CORRUPT);
    teardown();
}

static int locks, unlocks, depth, max_depth;
static void count_lock(void *ctx) { (void)ctx; locks++; if (++depth > max_depth) max_depth = depth; }
static void count_unlock(void *ctx) { (void)ctx; unlocks++; depth--; }

static void every_call_is_wrapped_in_the_lock(void)
{
    char buf[4];
    tfs_dir_t it;
    struct tfs_info info;
    setup(1024, 4, 4);
    cfg.lock = count_lock;
    cfg.unlock = count_unlock;
    locks = unlocks = depth = max_depth = 0;
    CHECK_EQ(tfs_format(&fs, &cfg), 0);
    CHECK_EQ(tfs_mount(&fs, &cfg), 0);
    CHECK_EQ(tfs_write(&fs, "f", "abc", 3), 0);
    CHECK_EQ(tfs_append(&fs, "f", "d", 1), 0);
    CHECK_EQ(tfs_size(&fs, "f"), 4);
    CHECK_EQ(tfs_read(&fs, "f", 0, buf, 4), 4);
    CHECK_EQ(tfs_dir_open(&fs, &it), 0);
    CHECK_EQ(tfs_dir_next(&fs, &it, &info), 1);
    CHECK_EQ(tfs_gc(&fs), 0);
    CHECK_EQ(tfs_remove(&fs, "f"), 0);
    CHECK_EQ(tfs_size(&fs, "nope"), TFS_ERR_NOENT);
    CHECK_EQ(locks, 10);                     /* dir_open touches no flash */
    CHECK_EQ(unlocks, locks);
    CHECK_EQ(max_depth, 1);
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
    RUN(dir_lists_live_files_with_sizes);
    RUN(sixty_fifth_file_is_rejected);
    RUN(churn_two_sectors);
    RUN(churn_eight_sectors);
    RUN(churn_prog16);
    RUN(full_fs_says_nospc_and_keeps_data);
    RUN(largest_file_rewrites_need_three_sectors);
    RUN(idle_gc_saves_the_next_write_from_collecting);
    RUN(bad_configs_are_rejected);
    RUN(geometry_mismatch_does_not_mount);
    RUN(flipped_bit_in_committed_data_is_reported);
    RUN(every_call_is_wrapped_in_the_lock);
}
