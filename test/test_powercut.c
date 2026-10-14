/* Power-loss contract: after a cut at any program/erase, the file system mounts
 * and every file is either its last committed content or, for the file being
 * changed when power failed, the content it would have had afterwards. */
#include <string.h>
#include "check.h"
#include "sim_flash.h"

#define MAX_FILES 3
#define MAX_LEN   400
#define N_OPS     120

enum { OP_WRITE, OP_APPEND, OP_REMOVE };

struct op {
    int type, file;
    uint32_t len;
    uint8_t seed;
};

struct file {
    int exists;
    uint32_t len;
    uint8_t data[MAX_LEN];
};

struct world {
    struct sim_flash sim;
    struct tfs_config cfg;
    tfs_t fs;
    struct file model[MAX_FILES];
    int nfiles;
    uint32_t maxlen;
};

static const char *names[MAX_FILES] = {"alpha", "b", "0123456789abcde"};

static uint32_t rng_next(uint32_t *r)
{
    *r ^= *r << 13;
    *r ^= *r >> 17;
    *r ^= *r << 5;
    return *r;
}

static void fill(uint8_t *buf, uint32_t len, uint8_t seed)
{
    uint32_t i;
    for (i = 0; i < len; i++)
        buf[i] = (uint8_t)(seed * 31 + i * 7);
}

static struct op make_op(const struct world *w, uint32_t *r)
{
    struct op o;
    uint32_t pick = rng_next(r) % 10;
    o.file = (int)(rng_next(r) % (uint32_t)w->nfiles);
    o.seed = (uint8_t)rng_next(r);
    o.type = pick < 5 ? OP_WRITE : pick < 8 ? OP_APPEND : OP_REMOVE;
    o.len = o.type == OP_APPEND ? 1 + rng_next(r) % 24 : rng_next(r) % (w->maxlen + 1);
    if (o.type == OP_APPEND && w->model[o.file].len + o.len > w->maxlen)
        o.type = OP_WRITE;
    return o;
}

/* The model after applying o to f. */
static void apply(struct file *f, const struct op *o)
{
    if (o->type == OP_REMOVE) {
        f->exists = 0;
        f->len = 0;
    } else if (o->type == OP_WRITE) {
        f->exists = 1;
        f->len = o->len;
        fill(f->data, o->len, o->seed);
    } else {
        if (!f->exists)
            f->len = 0;
        f->exists = 1;
        fill(f->data + f->len, o->len, o->seed);
        f->len += o->len;
    }
}

static int run_op(struct world *w, const struct op *o)
{
    uint8_t buf[MAX_LEN];
    const char *name = names[o->file];
    fill(buf, o->len, o->seed);
    if (o->type == OP_WRITE)
        return tfs_write(&w->fs, name, buf, o->len);
    if (o->type == OP_APPEND)
        return tfs_append(&w->fs, name, buf, o->len);
    return tfs_remove(&w->fs, name);
}

static int file_matches(struct world *w, int i, const struct file *f)
{
    uint8_t buf[MAX_LEN + 1];
    int size = tfs_size(&w->fs, names[i]);
    if (!f->exists)
        return size == TFS_ERR_NOENT;
    return size == (int)f->len && tfs_read(&w->fs, names[i], 0, buf, sizeof buf) == size
           && memcmp(buf, f->data, f->len) == 0;
}

static int all_match(struct world *w)
{
    tfs_dir_t it;
    struct tfs_info info;
    int i, n = 0, live = 0;
    for (i = 0; i < w->nfiles; i++) {
        if (!file_matches(w, i, &w->model[i]))
            return 0;
        live += w->model[i].exists;
    }
    tfs_dir_open(&w->fs, &it);
    while (tfs_dir_next(&w->fs, &it, &info) == 1)
        n++;
    return n == live;
}

static void world_init(struct world *w, uint32_t sector, uint32_t sectors, uint32_t prog,
                       int nfiles, uint32_t maxlen)
{
    memset(w, 0, sizeof *w);
    sim_init(&w->sim, sector, sectors, prog);
    sim_config(&w->sim, &w->cfg);
    w->nfiles = nfiles;
    w->maxlen = maxlen;
    tfs_format(&w->fs, &w->cfg);
    tfs_mount(&w->fs, &w->cfg);
}

/* Run op k; returns 0 if it completed (model updated, NOSPC leaves it alone),
 * 1 if power was cut during it (model untouched), -1 on an unexpected result. */
static int step(struct world *w, const struct op *o)
{
    int res = run_op(w, o);
    if (!w->sim.powered)
        return res == TFS_ERR_IO ? 1 : -1;
    if (res == TFS_ERR_NOSPC)
        return 0;
    if (res == TFS_ERR_NOENT && o->type == OP_REMOVE && !w->model[o->file].exists)
        return 0;
    if (res != 0) {
        printf("  op type %d file %d len %u: %d
", o->type, o->file, o->len, res);
        return -1;
    }
    apply(&w->model[o->file], o);
    return 0;
}

/* After a cut during o: power on, mount, check, and adopt whichever outcome
 * the interrupted file shows. Returns 0 on success. */
static int recover(struct world *w, const struct op *o)
{
    struct file after = w->model[o->file];
    int i;
    apply(&after, o);
    sim_power_on(&w->sim);
    if (tfs_mount(&w->fs, &w->cfg) != 0)
        return 1;
    for (i = 0; i < w->nfiles; i++)
        if (i != o->file && !file_matches(w, i, &w->model[i]))
            return 2;
    if (file_matches(w, o->file, &after))
        w->model[o->file] = after;
    else if (!file_matches(w, o->file, &w->model[o->file]))
        return 3;
    return all_match(w) ? 0 : 4;
}

/* Exhaustive: one run per possible cut point of a fixed workload. */
static void every_cut_point(uint32_t sector, uint32_t sectors, uint32_t prog, int nfiles,
                            uint32_t maxlen)
{
    static struct world w;
    struct op ops[N_OPS];
    uint32_t r = 12345;
    long total, cut, base_ops;
    int k, j, res, cuts = 0;

    world_init(&w, sector, sectors, prog, nfiles, maxlen);   /* dry run: count ops */
    base_ops = w.sim.ops;
    for (k = 0; k < N_OPS; k++) {
        ops[k] = make_op(&w, &r);
        CHECK_EQ(step(&w, &ops[k]), 0);
    }
    CHECK(all_match(&w));
    total = w.sim.ops - base_ops;
    CHECK(total > 100);
    sim_free(&w.sim);

    for (cut = 0; cut < total; cut++) {
        world_init(&w, sector, sectors, prog, nfiles, maxlen);
        sim_cut_after(&w.sim, cut, (uint32_t)cut);
        for (k = 0; k < N_OPS && (res = step(&w, &ops[k])) == 0; k++)
            ;
        if (k == N_OPS) {                      /* the cut fell after the workload */
            sim_free(&w.sim);
            continue;
        }
        if (res != 1) {
            printf("  cut %ld: op %d gave an unexpected result\n", cut, k);
            CHECK(0);
        }
        cuts++;
        if ((res = recover(&w, &ops[k])) != 0) {
            printf("  cut %ld (op %d type %d): recovery check %d failed\n", cut, k, ops[k].type, res);
            CHECK(0);
        }
        for (j = k + 1; j < N_OPS; j++) {      /* still fully usable afterwards */
            if (step(&w, &ops[j]) != 0)
                printf("  cut %ld (op %d type %d): op %d failed afterwards, violations %d
",
                       cut, k, ops[k].type, j, w.sim.violations);
            CHECK_EQ(w.sim.violations, 0);
        }
        CHECK(all_match(&w));
        CHECK_EQ(tfs_mount(&w.fs, &w.cfg), 0);
        CHECK(all_match(&w));
        CHECK_EQ(w.sim.violations, 0);
        sim_free(&w.sim);
    }
    CHECK(cuts > 100);
}

/* Fuzz: random workloads with repeated cuts, including cuts during recovery work. */
static void fuzz(uint32_t sector, uint32_t sectors, uint32_t prog, int nfiles, uint32_t maxlen,
                 uint32_t seed)
{
    static struct world w;
    struct op o;
    uint32_t r = seed;
    int k, res;
    world_init(&w, sector, sectors, prog, nfiles, maxlen);
    for (k = 0; k < 600; k++) {
        if (w.sim.ops_left < 0 && rng_next(&r) % 8 == 0)
            sim_cut_after(&w.sim, (long)(rng_next(&r) % 40), rng_next(&r));
        o = make_op(&w, &r);
        res = step(&w, &o);
        if (res == 1) {
            if ((res = recover(&w, &o)) != 0) {
                printf("  seed %u op %d: recovery check %d failed\n", seed, k, res);
                CHECK(0);
            }
        } else {
            CHECK_EQ(res, 0);
        }
    }
    sim_power_on(&w.sim);
    CHECK_EQ(tfs_mount(&w.fs, &w.cfg), 0);
    CHECK(all_match(&w));
    CHECK_EQ(w.sim.violations, 0);
    sim_free(&w.sim);
}

static void cuts_4x512_prog4(void) { every_cut_point(512, 4, 4, 3, 200); }
static void cuts_2x1024_prog4(void) { every_cut_point(1024, 2, 4, 2, 150); }
static void cuts_4x512_prog16(void) { every_cut_point(512, 4, 16, 3, 160); }
static void cuts_8x256_prog1(void) { every_cut_point(256, 8, 1, 3, 100); }

static void fuzz_all(void)
{
    uint32_t seed;
    for (seed = 1; seed <= 40; seed++) {
        fuzz(512, 4, 4, 3, 200, seed);
        fuzz(1024, 2, 8, 2, 150, seed);
        fuzz(256, 8, 16, 3, 60, seed);
    }
}

void powercut_tests(void)
{
    RUN(cuts_4x512_prog4);
    RUN(cuts_2x1024_prog4);
    RUN(cuts_4x512_prog16);
    RUN(cuts_8x256_prog1);
    RUN(fuzz_all);
}
