/* tiny-filesystem: a tiny power-loss-safe flash file system. See DESIGN.md. */
#include <string.h>
#include "tfs.h"

#define MAGIC     0x31534654u      /* "TFS1" */
#define VERSION   1
#define NONE      0xFFFFFFFFu
#define HDR_LEN   24               /* sector header */
#define REC_LEN   16               /* record header */
#define MAX_PROG  16
#define CHUNK     32

enum { K_WRITE = 0xA1, K_APPEND, K_DELETE, K_GCDONE };

static const uint8_t commit_pat[MAX_PROG] = {0x5A, 0xC3, 0xA5, 0x3C, 0x5A, 0xC3, 0xA5, 0x3C,
                                             0x5A, 0xC3, 0xA5, 0x3C, 0x5A, 0xC3, 0xA5, 0x3C};

struct rec {
    uint32_t addr, seq, len;
    uint16_t id;
    uint8_t kind, nlen;
};

typedef int (*rec_cb)(tfs_t *fs, const struct rec *r, void *ctx);

/* ---- helpers ---------------------------------------------------------- */

static uint32_t align_p(uint32_t p, uint32_t x) { return (x + p - 1) & ~(p - 1); }
static uint32_t al(const tfs_t *fs, uint32_t x) { return align_p(fs->cfg->prog_size, x); }
static uint32_t fp_of(const tfs_t *fs, uint32_t nlen, uint32_t len)
{
    return al(fs, REC_LEN + nlen + len) + fs->cfg->prog_size;
}
static uint32_t sec_addr(const tfs_t *fs, uint32_t idx)
{
    return fs->cfg->base + idx * fs->cfg->sector_size;
}
static uint32_t next_sec(const tfs_t *fs, uint32_t idx)
{
    return idx + 1 == fs->cfg->sector_count ? 0 : idx + 1;
}

static uint32_t get32(const uint8_t *b)
{
    return b[0] | (uint32_t)b[1] << 8 | (uint32_t)b[2] << 16 | (uint32_t)b[3] << 24;
}
static void put32(uint8_t *b, uint32_t v)
{
    b[0] = (uint8_t)v; b[1] = (uint8_t)(v >> 8); b[2] = (uint8_t)(v >> 16); b[3] = (uint8_t)(v >> 24);
}

static uint32_t crc_byte(uint32_t c, uint8_t b)
{
    int k;
    c ^= b;
    for (k = 0; k < 8; k++)
        c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1)));
    return c;
}
static uint32_t crc_buf(uint32_t c, const uint8_t *p, uint32_t n)
{
    while (n--)
        c = crc_byte(c, *p++);
    return c;
}

static int rd(tfs_t *fs, uint32_t addr, void *buf, uint32_t len)
{
    return fs->cfg->read(fs->cfg->ctx, addr, buf, len) ? TFS_ERR_IO : 0;
}

static int check_config(const struct tfs_config *cfg)
{
    uint32_t p = cfg->prog_size, s = cfg->sector_size;
    if (!cfg->read || !cfg->prog || !cfg->erase || cfg->sector_count < 2 || cfg->sector_count > 0xFFFF
        || p < 1 || p > MAX_PROG || (p & (p - 1)) || s < 128 || (s & (s - 1))
        || cfg->sector_count > (0xFFFFFFFFu - cfg->base) / s + 1)   /* region ends within 4 GB */
        return TFS_ERR_INVAL;
    return 0;
}

int tfs_max_file_size(const struct tfs_config *cfg)
{
    uint32_t p = cfg->prog_size, avail;
    if (check_config(cfg))
        return TFS_ERR_INVAL;
    /* sector header, reserved GC_DONE and DELETE records, commit unit; the
     * rest holds a record header + longest name + payload */
    avail = cfg->sector_size - align_p(p, HDR_LEN) - 2 * (align_p(p, REC_LEN) + p) - p;
    return (int)((avail & ~(p - 1)) - REC_LEN - TFS_NAME_MAX);
}

/* ---- record writer ---------------------------------------------------- */

struct wr {
    tfs_t *fs;
    uint32_t addr, crc;
    uint8_t n, dry, err;
    uint8_t buf[MAX_PROG];
};

/* A dry writer only computes the CRC of what is put. */
static void wr_init(struct wr *w, tfs_t *fs, uint32_t addr, uint8_t dry)
{
    memset(w, 0, sizeof *w);
    w->fs = fs;
    w->addr = addr;
    w->crc = NONE;
    w->dry = dry;
}

static void wr_put(struct wr *w, const void *src, uint32_t len)
{
    const uint8_t *p = src;
    const struct tfs_config *cfg = w->fs->cfg;
    while (len--) {
        w->crc = crc_byte(w->crc, *p);
        w->buf[w->n++] = *p++;
        if (w->n == cfg->prog_size) {
            if (!w->dry && !w->err && cfg->prog(cfg->ctx, w->addr, w->buf, w->n))
                w->err = 1;
            w->addr += w->n;
            w->n = 0;
        }
    }
}

static void wr_flash(struct wr *w, uint32_t addr, uint32_t len)
{
    uint8_t tmp[CHUNK];
    while (len) {
        uint32_t n = len < CHUNK ? len : CHUNK;
        if (rd(w->fs, addr, tmp, n))
            w->err = 1;
        wr_put(w, tmp, n);
        addr += n;
        len -= n;
    }
}

/* Pad the current program unit with 0xFF. */
static void wr_pad(struct wr *w)
{
    static const uint8_t ff = 0xFF;
    while (w->n)
        wr_put(w, &ff, 1);
}

/* Pad, then program the commit unit. */
static void wr_commit(struct wr *w)
{
    wr_pad(w);
    wr_put(w, commit_pat, w->fs->cfg->prog_size);
}

/* Payload of a record being written: the current content of the file whose
 * base is `base` (if any, `base_size` bytes), followed by `len` bytes of data. */
struct src {
    struct rec *base;
    uint32_t base_size;
    const void *data;
    uint32_t len;
};

static int walk(tfs_t *fs, const struct rec *base,
                int (*fn)(tfs_t *, uint32_t, uint32_t, void *), void *ctx);

static int feed_seg(tfs_t *fs, uint32_t addr, uint32_t len, void *ctx)
{
    (void)fs;
    wr_flash(ctx, addr, len);
    return 0;
}

static void feed(struct wr *w, const struct src *s)
{
    if (s->base && walk(w->fs, s->base, feed_seg, w))
        w->err = 1;
    wr_put(w, s->data, s->len);
}

/* Write a record at the head. Caller has ensured it fits. */
static int put_record(tfs_t *fs, uint8_t kind, uint16_t id, const char *name, uint8_t nlen,
                      const struct src *s)
{
    struct wr w;
    uint8_t h[REC_LEN], dry;
    h[0] = kind;
    h[1] = nlen;
    h[2] = (uint8_t)id;
    h[3] = (uint8_t)(id >> 8);
    put32(h + 4, fs->next_seq);
    put32(h + 8, s->base_size + s->len);
    for (dry = 1;; dry = 0) {                  /* dry pass: CRC only */
        wr_init(&w, fs, sec_addr(fs, fs->head) + fs->head_off, dry);
        wr_put(&w, h, dry ? 12 : REC_LEN);
        wr_put(&w, name, nlen);
        feed(&w, s);
        if (!dry)
            break;
        put32(h + 12, ~w.crc);
    }
    wr_commit(&w);
    if (w.err) {
        fs->retired = 1;                       /* never write after a failed record */
        return TFS_ERR_IO;
    }
    fs->head_off = w.addr - sec_addr(fs, fs->head);
    fs->next_seq++;
    return 0;
}

/* ---- sectors ---------------------------------------------------------- */

/* Sector header: magic, geometry, version, seq, gc_src, CRC of the first 20 bytes. */
static void make_hdr(const tfs_t *fs, uint8_t *h, uint32_t seq, uint32_t gc_src)
{
    const struct tfs_config *cfg = fs->cfg;
    put32(h, MAGIC);
    put32(h + 4, cfg->sector_size);
    h[8] = (uint8_t)cfg->sector_count;
    h[9] = (uint8_t)(cfg->sector_count >> 8);
    h[10] = VERSION;
    h[11] = (uint8_t)cfg->prog_size;
    put32(h + 12, seq);
    put32(h + 16, gc_src);
    put32(h + 20, ~crc_buf(NONE, h, 20));
}

/* 1 = valid header (seq/gc_src filled in), 0 = not a valid sector, < 0 = error. */
static int read_hdr(tfs_t *fs, uint32_t idx, uint32_t *seq, uint32_t *gc_src)
{
    uint8_t h[HDR_LEN], want[HDR_LEN];
    if (rd(fs, sec_addr(fs, idx), h, HDR_LEN))
        return TFS_ERR_IO;
    *seq = get32(h + 12);
    *gc_src = get32(h + 16);
    make_hdr(fs, want, *seq, *gc_src);
    return memcmp(h, want, HDR_LEN) == 0;
}

/* Make sector idx the new head: erase it, then program its header. The erase
 * is unconditional: flash that reads 0xFF may still hold a program cut short
 * by power loss, and must not be programmed again. */
static int activate(tfs_t *fs, uint32_t idx, uint32_t gc_src)
{
    const struct tfs_config *cfg = fs->cfg;
    struct wr w;
    uint8_t h[HDR_LEN];
    if (cfg->erase(cfg->ctx, sec_addr(fs, idx)))
        return TFS_ERR_IO;
    make_hdr(fs, h, fs->sector_seq + 1, gc_src);
    wr_init(&w, fs, sec_addr(fs, idx), 0);
    wr_put(&w, h, HDR_LEN);
    wr_pad(&w);
    if (w.err)
        return TFS_ERR_IO;
    fs->sector_seq++;
    fs->head = (uint16_t)idx;
    fs->head_off = al(fs, HDR_LEN);
    fs->retired = 0;
    fs->gc_done = 0;
    return 0;
}

/* Walk the committed records of sector idx, calling cb (if any) on each; a
 * non-zero cb result stops the walk and is returned. The walk ends at erased
 * flash or at the first record that is not committed. With verify, a committed
 * record whose CRC does not match returns TFS_ERR_CORRUPT. */
static int scan_sector(tfs_t *fs, uint32_t idx, rec_cb cb, void *ctx, int verify)
{
    const struct tfs_config *cfg = fs->cfg;
    uint32_t base = sec_addr(fs, idx), off = al(fs, HDR_LEN), fp;
    uint8_t h[REC_LEN], u[MAX_PROG];
    struct rec r;
    struct wr w;
    int res, bad;
    while (off + fp_of(fs, 0, 0) <= cfg->sector_size) {
        if (rd(fs, base + off, h, REC_LEN))         /* erased flash fails the checks */
            return TFS_ERR_IO;
        r.addr = base + off;
        r.kind = h[0];
        r.nlen = h[1];
        r.id = (uint16_t)(h[2] | h[3] << 8);
        r.seq = get32(h + 4);
        r.len = get32(h + 8);
        bad = r.kind < K_WRITE || r.kind > K_GCDONE || r.nlen > TFS_NAME_MAX
              || (r.kind == K_WRITE) != (r.nlen > 0) || r.len > cfg->sector_size
              || r.id >= TFS_MAX_FILES;
        fp = bad ? 0 : fp_of(fs, r.nlen, r.len);
        if (!bad && off + fp <= cfg->sector_size) {
            if (rd(fs, base + off + fp - cfg->prog_size, u, cfg->prog_size))
                return TFS_ERR_IO;
            bad = memcmp(u, commit_pat, cfg->prog_size) != 0;
        } else {
            bad = 1;
        }
        if (bad)
            break;
        if (verify) {
            wr_init(&w, fs, 0, 1);
            wr_put(&w, h, 12);
            wr_flash(&w, r.addr + REC_LEN, r.nlen + r.len);
            if (w.err)
                return TFS_ERR_IO;
            if (~w.crc != get32(h + 12))
                return TFS_ERR_CORRUPT;
        }
        if (cb && (res = cb(fs, &r, ctx)) != 0)
            return res;
        off += fp;
    }
    return 0;
}

/* Walk every committed record of the log, oldest first. */
static int scan(tfs_t *fs, rec_cb cb, void *ctx)
{
    uint32_t i = fs->oldest;
    int res;
    for (;;) {
        if ((res = scan_sector(fs, i, cb, ctx, 0)) != 0)
            return res;
        if (i == fs->head)
            return 0;
        i = next_sec(fs, i);
    }
}

static uint32_t used_sectors(const tfs_t *fs)
{
    uint32_t n = fs->cfg->sector_count;
    return (fs->head + n - fs->oldest) % n + 1;
}

static int gc_step(tfs_t *fs);

/* Until a sector holds its GC_DONE record it keeps room for one (so a GC into
 * it always completes), and all but DELETE records also leave room for one
 * DELETE (so a full file system can always remove a file). Records other than
 * GC_DONE and DELETE may therefore use this much of a sector: */
static uint32_t usable(const tfs_t *fs)
{
    return fs->cfg->sector_size - al(fs, HDR_LEN) - 2 * fp_of(fs, 0, 0);
}

/* Room for live records: every sector but the one kept free for GC. */
static int fits(const tfs_t *fs, uint32_t fp)
{
    return fs->live + fp <= (fs->cfg->sector_count - 1) * usable(fs);
}

/* Make room for a record of footprint fp at the head, advancing the head and
 * collecting garbage as needed. */
static int ensure_space(tfs_t *fs, uint32_t fp, int is_delete)
{
    const struct tfs_config *cfg = fs->cfg;
    uint32_t tries, reserve;
    int res;
    for (tries = 0;; tries++) {
        reserve = (uint32_t)(!is_delete + !fs->gc_done) * fp_of(fs, 0, 0);
        if (!fs->retired && fs->head_off + fp + reserve <= cfg->sector_size)
            return 0;
        if (tries > cfg->sector_count)
            return TFS_ERR_NOSPC;
        if (used_sectors(fs) + 1 < cfg->sector_count)
            res = activate(fs, next_sec(fs, fs->head), NONE);
        else
            res = gc_step(fs);
        if (res)
            return res;
    }
}

/* ---- lookup ----------------------------------------------------------- */

static int check_name(const char *name, uint8_t *nlen)
{
    size_t n;
    if (!name)
        return TFS_ERR_INVAL;
    n = strlen(name);
    if (n == 0)
        return TFS_ERR_INVAL;
    if (n > TFS_NAME_MAX)
        return TFS_ERR_NAMETOOLONG;
    *nlen = (uint8_t)n;
    return 0;
}

struct find {
    const char *name;
    uint8_t nlen, found;
    struct rec base;
};

static int find_cb(tfs_t *fs, const struct rec *r, void *ctx)
{
    struct find *f = ctx;
    char tmp[TFS_NAME_MAX];
    if (r->kind != K_WRITE && r->kind != K_DELETE)
        return 0;
    if (f->found && r->id == f->base.id)
        f->found = 0;                         /* superseded */
    if (r->kind == K_WRITE && r->nlen == f->nlen) {
        if (rd(fs, r->addr + REC_LEN, tmp, r->nlen))
            return TFS_ERR_IO;
        if (memcmp(tmp, f->name, r->nlen) == 0) {
            f->found = 1;
            f->base = *r;
        }
    }
    return 0;
}

/* Find the live file called name: 1 = found (f->base is its base WRITE),
 * TFS_ERR_NOENT = none. */
static int find(tfs_t *fs, const char *name, struct find *f)
{
    int res;
    memset(f, 0, sizeof *f);
    f->name = name;
    if ((res = check_name(name, &f->nlen)) != 0 || (res = scan(fs, find_cb, f)) != 0)
        return res;
    return f->found ? 1 : TFS_ERR_NOENT;
}

static int ids_cb(tfs_t *fs, const struct rec *r, void *ctx)
{
    uint8_t *live = ctx;
    (void)fs;
    if (r->kind == K_WRITE)
        live[r->id >> 3] |= (uint8_t)(1u << (r->id & 7));
    else if (r->kind == K_DELETE)
        live[r->id >> 3] &= (uint8_t)~(1u << (r->id & 7));
    return 0;
}

/* Bitmap of live file ids. */
static int live_ids(tfs_t *fs, uint8_t live[TFS_MAX_FILES / 8])
{
    memset(live, 0, TFS_MAX_FILES / 8);
    return scan(fs, ids_cb, live);
}

/* The appends that belong to the file whose base is c->base: how many, how many
 * bytes, and the first one with seq > c->after. */
struct chain {
    const struct rec *base;
    uint32_t after, count, bytes, fps;
    uint8_t found;
    struct rec next;
};

static int chain_cb(tfs_t *fs, const struct rec *r, void *ctx)
{
    struct chain *c = ctx;
    if (r->kind != K_APPEND || r->id != c->base->id || r->seq < c->base->seq)
        return 0;
    c->count++;
    c->bytes += r->len;
    c->fps += fp_of(fs, 0, r->len);
    if (r->seq > c->after && (!c->found || r->seq < c->next.seq)) {
        c->found = 1;
        c->next = *r;
    }
    return 0;
}

static int chain(tfs_t *fs, const struct rec *base, uint32_t after, struct chain *c)
{
    memset(c, 0, sizeof *c);
    c->base = base;
    c->after = after;
    return scan(fs, chain_cb, c);
}

/* Call fn on each piece of the file's content in order: base payload, then appends. */
static int walk(tfs_t *fs, const struct rec *base,
                int (*fn)(tfs_t *, uint32_t, uint32_t, void *), void *ctx)
{
    struct chain c;
    int res = fn(fs, base->addr + REC_LEN + base->nlen, base->len, ctx);
    uint32_t after = base->seq;
    while (!res) {
        if ((res = chain(fs, base, after, &c)) != 0 || !c.found)
            break;
        res = fn(fs, c.next.addr + REC_LEN, c.next.len, ctx);
        after = c.next.seq;
    }
    return res;
}

/* Size of the file whose base is `base`; optionally its append count and the
 * footprint of all its records. */
static int file_size(tfs_t *fs, const struct rec *base, uint32_t *size, uint32_t *appends,
                     uint32_t *fp)
{
    struct chain c;
    int res = chain(fs, base, base->seq, &c);
    *size = base->len + c.bytes;
    if (appends)
        *appends = c.count;
    if (fp)
        *fp = fp_of(fs, base->nlen, base->len) + c.fps;
    return res;
}

/* ---- garbage collection ----------------------------------------------- */

#define BATCH 16

/* Base (newest WRITE/DELETE) of each id in [first, first + BATCH). */
struct bases {
    uint32_t first;
    uint32_t seq[BATCH];
    uint8_t kind[BATCH];
};

static int bases_cb(tfs_t *fs, const struct rec *r, void *ctx)
{
    struct bases *b = ctx;
    uint32_t i = (uint32_t)r->id - b->first;
    (void)fs;
    if ((r->kind == K_WRITE || r->kind == K_DELETE) && i < BATCH) {
        b->seq[i] = r->seq;
        b->kind[i] = r->kind;
    }
    return 0;
}

/* Copy r verbatim to the head. */
static int copy_record(tfs_t *fs, const struct rec *r)
{
    struct wr w;
    uint32_t fp = fp_of(fs, r->nlen, r->len);
    wr_init(&w, fs, sec_addr(fs, fs->head) + fs->head_off, 0);
    wr_flash(&w, r->addr, fp - fs->cfg->prog_size);
    wr_commit(&w);
    if (w.err)
        return TFS_ERR_IO;
    fs->head_off += fp;
    return 0;
}

/* Is r a live record of an id in this batch? DELETE and GC_DONE never are. */
static int is_live(const struct bases *b, const struct rec *r)
{
    uint32_t i = (uint32_t)r->id - b->first;
    return i < BATCH && b->kind[i] == K_WRITE
           && ((r->kind == K_WRITE && r->seq == b->seq[i]) || (r->kind == K_APPEND && r->seq > b->seq[i]));
}

static int copy_live_cb(tfs_t *fs, const struct rec *r, void *ctx)
{
    return is_live(ctx, r) ? copy_record(fs, r) : 0;
}

static int count_live_cb(tfs_t *fs, const struct rec *r, void *ctx)
{
    if (is_live(ctx, r))
        fs->live += fp_of(fs, r->nlen, r->len);
    return 0;
}

static int base_batch(tfs_t *fs, struct bases *b)
{
    memset(b->kind, 0, sizeof b->kind);
    return scan(fs, bases_cb, b);
}

/* Compact the oldest sector into the free sector after the head, which then
 * leaves the oldest sector free. Only called when exactly one sector is free. */
static int gc_step(tfs_t *fs)
{
    struct bases b;
    struct src none = {0, 0, 0, 0};
    uint32_t src = fs->oldest, seq, gc_src;
    int res;
    if ((res = read_hdr(fs, src, &seq, &gc_src)) != 1)
        return res < 0 ? res : TFS_ERR_CORRUPT;
    if ((res = activate(fs, next_sec(fs, fs->head), seq)) != 0)
        return res;
    for (b.first = 0; b.first < TFS_MAX_FILES; b.first += BATCH)
        if ((res = base_batch(fs, &b)) != 0
            || (res = scan_sector(fs, src, copy_live_cb, &b, 0)) != 0)
            return res;
    if ((res = put_record(fs, K_GCDONE, 0, 0, 0, &none)) != 0)
        return res;
    fs->gc_done = 1;
    fs->oldest = (uint16_t)next_sec(fs, src);  /* src is now the free sector; it is */
    return 0;                                  /* erased when next activated */
}

/* ---- public API ------------------------------------------------------- */

static void lock(tfs_t *fs)
{
    if (fs->cfg->lock)
        fs->cfg->lock(fs->cfg->ctx);
}
static int unlock(tfs_t *fs, int res)
{
    if (fs->cfg->unlock)
        fs->cfg->unlock(fs->cfg->ctx);
    return res;
}

int tfs_format(tfs_t *fs, const struct tfs_config *cfg)
{
    uint32_t i;
    int res;
    if ((res = check_config(cfg)) != 0)
        return res;
    memset(fs, 0, sizeof *fs);
    fs->cfg = cfg;
    lock(fs);
    for (i = 0; i < cfg->sector_count; i++)
        if (cfg->erase(cfg->ctx, sec_addr(fs, i)))
            return unlock(fs, TFS_ERR_IO);
    return unlock(fs, activate(fs, 0, NONE));
}

static int max_seq_cb(tfs_t *fs, const struct rec *r, void *ctx)
{
    (void)fs;
    if (r->seq >= *(uint32_t *)ctx)
        *(uint32_t *)ctx = r->seq + 1;
    return 0;
}

static int gcdone_cb(tfs_t *fs, const struct rec *r, void *ctx)
{
    (void)fs;
    (void)ctx;
    return r->kind == K_GCDONE;
}

static int do_mount(tfs_t *fs)
{
    const struct tfs_config *cfg = fs->cfg;
    struct bases b;
    uint32_t i, seq, gc_src, hi = 0, top = NONE, hi2 = 0, top2 = NONE;
    int res;
    /* The head is the newest valid sector, unless that is a GC target whose GC
     * never finished; then the log ends at the second newest. */
    for (i = 0; i < cfg->sector_count; i++) {
        if ((res = read_hdr(fs, i, &seq, &gc_src)) < 0)
            return res;
        if (res && seq >= hi2) {
            if (seq >= hi) {
                hi2 = hi;
                top2 = top;
                hi = seq;
                top = i;
            } else {
                hi2 = seq;
                top2 = i;
            }
        }
    }
    fs->sector_seq = hi;
    for (;;) {
        if (top == NONE)
            return TFS_ERR_CORRUPT;
        if ((res = read_hdr(fs, top, &seq, &gc_src)) < 0)
            return res;
        if (gc_src == NONE || top == top2)
            break;
        if ((res = scan_sector(fs, top, gcdone_cb, 0, 0)) < 0)
            return res;
        if (res)
            break;
        top = top2;
        hi = hi2;
    }
    fs->head = (uint16_t)top;
    fs->gc_done = gc_src != NONE;
    /* The log runs back from the head while sequence numbers fall. The sector
     * after the head is always free: never formatted into, a GC source, or a
     * GC target that never finished. */
    fs->oldest = (uint16_t)top;
    for (i = top ? top - 1 : cfg->sector_count - 1; i != next_sec(fs, top);
         i = i ? i - 1 : cfg->sector_count - 1) {
        if ((res = read_hdr(fs, i, &seq, &gc_src)) < 0)
            return res;
        if (!res || seq >= hi)
            break;
        hi = seq;
        fs->oldest = (uint16_t)i;
    }
    fs->next_seq = 1;
    for (i = fs->oldest;; i = next_sec(fs, i)) {
        if ((res = scan_sector(fs, i, max_seq_cb, &fs->next_seq, 1)) != 0)
            return res;
        if (i == fs->head)
            break;
    }
    /* Flash after the data in the head may hold a program that power loss cut
     * short before it changed any bit: it reads as erased but must not be
     * programmed again. So writing resumes in a freshly erased sector. */
    fs->retired = 1;
    for (b.first = 0; b.first < TFS_MAX_FILES; b.first += BATCH)
        if ((res = base_batch(fs, &b)) != 0 || (res = scan(fs, count_live_cb, &b)) != 0)
            return res;
    return 0;
}

int tfs_mount(tfs_t *fs, const struct tfs_config *cfg)
{
    int res;
    if ((res = check_config(cfg)) != 0)
        return res;
    memset(fs, 0, sizeof *fs);
    fs->cfg = cfg;
    lock(fs);
    return unlock(fs, do_mount(fs));
}

int tfs_size(tfs_t *fs, const char *name)
{
    struct find f;
    uint32_t size;
    int res;
    lock(fs);
    if ((res = find(fs, name, &f)) > 0 && (res = file_size(fs, &f.base, &size, 0, 0)) == 0)
        res = (int)size;
    return unlock(fs, res);
}

struct rd_ctx {
    uint32_t off, len, done;
    uint8_t *buf;
};

static int read_seg(tfs_t *fs, uint32_t addr, uint32_t len, void *ctx)
{
    struct rd_ctx *r = ctx;
    uint32_t n;
    if (r->off >= len) {
        r->off -= len;
        return 0;
    }
    n = len - r->off;
    if (n > r->len - r->done)
        n = r->len - r->done;
    if (rd(fs, addr + r->off, r->buf + r->done, n))
        return TFS_ERR_IO;
    r->off = 0;
    r->done += n;
    return r->done == r->len;              /* 1 stops the walk: buffer full */
}

static int do_read(tfs_t *fs, const char *name, uint32_t off, void *buf, uint32_t len)
{
    struct find f;
    struct rd_ctx r;
    int res = find(fs, name, &f);
    if (res < 0)
        return res;
    if (!buf && len)
        return TFS_ERR_INVAL;
    if (!len)
        return 0;
    r.off = off;
    r.len = len;
    r.done = 0;
    r.buf = buf;
    res = walk(fs, &f.base, read_seg, &r);
    return res < 0 ? res : (int)r.done;
}

int tfs_read(tfs_t *fs, const char *name, uint32_t off, void *buf, uint32_t len)
{
    lock(fs);
    return unlock(fs, do_read(fs, name, off, buf, len));
}

static int base_cb(tfs_t *fs, const struct rec *r, void *ctx)
{
    struct rec *base = ctx;
    (void)fs;
    if ((r->kind == K_WRITE || r->kind == K_DELETE) && r->id == base->id)
        *base = *r;
    return 0;
}

/* Write a record at the head, making room first. It replaces old_fp bytes of
 * live records (the file's previous ones); a DELETE is not itself live. */
static int emit(tfs_t *fs, uint8_t kind, uint16_t id, const char *name, uint8_t nlen,
                struct src *s, uint32_t old_fp)
{
    uint32_t fp = fp_of(fs, nlen, s->base_size + s->len);
    int del = kind == K_DELETE, res;
    if (!del && !fits(fs, fp))
        return TFS_ERR_NOSPC;
    if ((res = ensure_space(fs, fp, del)) != 0)
        return res;
    if (s->base && (res = scan(fs, base_cb, s->base)) != 0)   /* GC may have moved it */
        return res;
    if ((res = put_record(fs, kind, id, name, nlen, s)) != 0)
        return res;
    fs->live += (del ? 0 : fp) - old_fp;
    return 0;
}

#define MAX_APPENDS 8

/* tfs_write (append = 0) and tfs_append. */
static int do_put(tfs_t *fs, const char *name, const void *buf, uint32_t len, int append)
{
    struct find f;
    struct src s = {0, 0, 0, 0};
    uint8_t live[TFS_MAX_FILES / 8];
    uint32_t size = 0, appends = 0, old_fp = 0;
    uint16_t id = 0;
    int found = find(fs, name, &f), res;
    if (found < 0 && found != TFS_ERR_NOENT)
        return found;
    found = found > 0;
    if (!buf && len)
        return TFS_ERR_INVAL;
    s.data = buf;
    s.len = len;
    if (found) {
        id = f.base.id;
        if ((res = file_size(fs, &f.base, &size, &appends, &old_fp)) != 0)
            return res;
    }
    if (!append)
        size = 0;
    if (len > (uint32_t)tfs_max_file_size(fs->cfg) - size)
        return TFS_ERR_FBIG;
    if (!found) {
        if ((res = live_ids(fs, live)) != 0)
            return res;
        for (id = 0; id < TFS_MAX_FILES && (live[id >> 3] >> (id & 7) & 1); id++)
            ;
        if (id == TFS_MAX_FILES)
            return TFS_ERR_NOFILES;
    }
    if (found && append) {
        if (appends < MAX_APPENDS)
            return emit(fs, K_APPEND, id, 0, 0, &s, 0);
        s.base = &f.base;                     /* chain is long: write old content + */
        s.base_size = size;                   /* new data as one WRITE record */
    }
    return emit(fs, K_WRITE, id, name, f.nlen, &s, old_fp);
}

int tfs_write(tfs_t *fs, const char *name, const void *buf, uint32_t len)
{
    lock(fs);
    return unlock(fs, do_put(fs, name, buf, len, 0));
}

int tfs_append(tfs_t *fs, const char *name, const void *buf, uint32_t len)
{
    lock(fs);
    return unlock(fs, do_put(fs, name, buf, len, 1));
}

static int do_remove(tfs_t *fs, const char *name)
{
    struct find f;
    struct src s = {0, 0, 0, 0};
    uint32_t size, old_fp;
    int res = find(fs, name, &f);
    if (res < 0)
        return res;
    if ((res = file_size(fs, &f.base, &size, 0, &old_fp)) != 0)
        return res;
    return emit(fs, K_DELETE, f.base.id, 0, 0, &s, old_fp);
}

int tfs_remove(tfs_t *fs, const char *name)
{
    lock(fs);
    return unlock(fs, do_remove(fs, name));
}

int tfs_dir_open(tfs_t *fs, tfs_dir_t *it)
{
    (void)fs;
    it->next_id = 0;
    return 0;
}

static int do_dir_next(tfs_t *fs, tfs_dir_t *it, struct tfs_info *info)
{
    uint8_t live[TFS_MAX_FILES / 8];
    struct rec base;
    uint32_t id = it->next_id;
    int res = live_ids(fs, live);
    if (res)
        return res;
    while (id < TFS_MAX_FILES && !(live[id >> 3] >> (id & 7) & 1))
        id++;
    it->next_id = (uint8_t)(id < TFS_MAX_FILES ? id + 1 : id);
    if (id == TFS_MAX_FILES)
        return 0;
    base.id = (uint16_t)id;
    if ((res = scan(fs, base_cb, &base)) != 0
        || (res = rd(fs, base.addr + REC_LEN, info->name, base.nlen)) != 0
        || (res = file_size(fs, &base, &info->size, 0, 0)) != 0)
        return res;
    info->name[base.nlen] = 0;
    return 1;
}

int tfs_dir_next(tfs_t *fs, tfs_dir_t *it, struct tfs_info *info)
{
    lock(fs);
    return unlock(fs, do_dir_next(fs, it, info));
}

/* Pay for the next GC now: if the head is (nearly) full and the next head
 * advance would need a GC, run it. Returns 1 if a GC ran, 0 if not needed. */
int tfs_gc(tfs_t *fs)
{
    const struct tfs_config *cfg = fs->cfg;
    int res = 0;
    lock(fs);
    if (used_sectors(fs) + 1 == cfg->sector_count
        && (fs->retired || cfg->sector_size - fs->head_off < cfg->sector_size / 4))
        if ((res = gc_step(fs)) == 0)
            res = 1;
    return unlock(fs, res);
}
