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

static const uint8_t commit_pat[4] = {0x5A, 0xC3, 0xA5, 0x3C};

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

static int crc_flash(tfs_t *fs, uint32_t *c, uint32_t addr, uint32_t len)
{
    uint8_t tmp[CHUNK];
    while (len) {
        uint32_t n = len < CHUNK ? len : CHUNK;
        if (rd(fs, addr, tmp, n))
            return TFS_ERR_IO;
        *c = crc_buf(*c, tmp, n);
        addr += n;
        len -= n;
    }
    return 0;
}

static int check_config(const struct tfs_config *cfg)
{
    uint32_t p = cfg->prog_size, s = cfg->sector_size;
    if (!cfg->read || !cfg->prog || !cfg->erase || cfg->sector_count < 2 || cfg->sector_count > 0xFFFF
        || p < 1 || p > MAX_PROG || (p & (p - 1)) || s < 128 || (s & (s - 1)))
        return TFS_ERR_INVAL;
    return 0;
}

int tfs_max_file_size(const struct tfs_config *cfg)
{
    uint32_t p = cfg->prog_size, avail;
    if (check_config(cfg))
        return TFS_ERR_INVAL;
    /* sector header, reserved GC_DONE record, commit unit; the rest holds
     * a record header + longest name + payload */
    avail = cfg->sector_size - align_p(p, HDR_LEN) - (align_p(p, REC_LEN) + p) - p;
    return (int)((avail & ~(p - 1)) - REC_LEN - TFS_NAME_MAX);
}

/* ---- record writer ---------------------------------------------------- */

struct wr {
    tfs_t *fs;
    uint32_t addr, crc;
    uint8_t n, dry, err;
    uint8_t buf[MAX_PROG];
};

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

/* Pad the current program unit with 0xFF, then program the commit unit. */
static void wr_commit(struct wr *w)
{
    static const uint8_t ff = 0xFF;
    uint32_t i;
    while (w->n)
        wr_put(w, &ff, 1);
    for (i = 0; i < w->fs->cfg->prog_size; i++)
        wr_put(w, &commit_pat[i & 3], 1);
}

/* Payload of a record being written: optional data from RAM. */
struct src {
    const void *data;
    uint32_t len;
};

static void feed(struct wr *w, const struct src *s)
{
    wr_put(w, s->data, s->len);
}

/* Write a record at the head. Caller has ensured it fits. */
static int put_record(tfs_t *fs, uint8_t kind, uint16_t id, const char *name, uint8_t nlen,
                      const struct src *s)
{
    struct wr w;
    uint8_t h[REC_LEN];
    h[0] = kind;
    h[1] = nlen;
    h[2] = (uint8_t)id;
    h[3] = (uint8_t)(id >> 8);
    put32(h + 4, fs->next_seq);
    put32(h + 8, s->len);

    memset(&w, 0, sizeof w);                   /* dry pass: CRC only */
    w.fs = fs;
    w.dry = 1;
    w.crc = NONE;
    wr_put(&w, h, 12);
    wr_put(&w, name, nlen);
    feed(&w, s);
    put32(h + 12, ~w.crc);

    memset(&w, 0, sizeof w);
    w.fs = fs;
    w.addr = sec_addr(fs, fs->head) + fs->head_off;
    wr_put(&w, h, REC_LEN);
    wr_put(&w, name, nlen);
    feed(&w, s);
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

/* 1 = valid header (seq/gc_src filled in), 0 = not a valid sector, < 0 = error. */
static int read_hdr(tfs_t *fs, uint32_t idx, uint32_t *seq, uint32_t *gc_src)
{
    const struct tfs_config *cfg = fs->cfg;
    uint8_t h[HDR_LEN];
    if (rd(fs, sec_addr(fs, idx), h, HDR_LEN))
        return TFS_ERR_IO;
    if (get32(h) != MAGIC || get32(h + 20) != ~crc_buf(NONE, h, 20) || get32(h + 8) != cfg->sector_size
        || (h[12] | h[13] << 8) != cfg->sector_count || h[14] != VERSION || h[15] != cfg->prog_size)
        return 0;
    *seq = get32(h + 4);
    *gc_src = get32(h + 16);
    return 1;
}

/* Make sector idx the new head: erase it unless it reads fully erased, then
 * program its header. */
static int activate(tfs_t *fs, uint32_t idx, uint32_t gc_src)
{
    const struct tfs_config *cfg = fs->cfg;
    struct wr w;
    uint8_t h[HDR_LEN], tmp[CHUNK];
    uint32_t off, i;
    for (off = 0; off < cfg->sector_size; off += CHUNK) {
        if (rd(fs, sec_addr(fs, idx) + off, tmp, CHUNK))
            return TFS_ERR_IO;
        for (i = 0; i < CHUNK && tmp[i] == 0xFF; i++)
            ;
        if (i < CHUNK) {
            if (cfg->erase(cfg->ctx, sec_addr(fs, idx)))
                return TFS_ERR_IO;
            break;
        }
    }
    put32(h, MAGIC);
    put32(h + 4, fs->sector_seq + 1);
    put32(h + 8, cfg->sector_size);
    h[12] = (uint8_t)cfg->sector_count;
    h[13] = (uint8_t)(cfg->sector_count >> 8);
    h[14] = VERSION;
    h[15] = (uint8_t)cfg->prog_size;
    put32(h + 16, gc_src);
    put32(h + 20, ~crc_buf(NONE, h, 20));
    memset(&w, 0, sizeof w);
    w.fs = fs;
    w.addr = sec_addr(fs, idx);
    wr_put(&w, h, HDR_LEN);
    while (w.n)
        wr_put(&w, "\xFF", 1);
    if (w.err)
        return TFS_ERR_IO;
    fs->sector_seq++;
    fs->head = (uint16_t)idx;
    fs->head_off = al(fs, HDR_LEN);
    fs->retired = 0;
    return 0;
}

/* Walk the committed records of sector idx, calling cb (if any) on each; a
 * non-zero cb result stops the walk and is returned. On return *end is the
 * offset after the last committed record and *torn tells whether the walk
 * stopped at something that is neither a record nor erased flash. With verify,
 * a committed record whose CRC does not match returns TFS_ERR_CORRUPT. */
static int scan_sector(tfs_t *fs, uint32_t idx, rec_cb cb, void *ctx, uint32_t *end, int *torn,
                       int verify)
{
    const struct tfs_config *cfg = fs->cfg;
    uint32_t base = sec_addr(fs, idx), off = al(fs, HDR_LEN), i, fp, c;
    uint8_t h[REC_LEN], u[MAX_PROG];
    struct rec r;
    int res, bad;
    if (torn)
        *torn = 0;
    while (off + fp_of(fs, 0, 0) <= cfg->sector_size) {
        if (rd(fs, base + off, h, REC_LEN))
            return TFS_ERR_IO;
        for (i = 0; i < REC_LEN && h[i] == 0xFF; i++)
            ;
        if (i == REC_LEN)
            break;                                  /* erased: end of data */
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
            for (i = 0; i < cfg->prog_size && u[i] == commit_pat[i & 3]; i++)
                ;
            bad = i < cfg->prog_size;
        } else {
            bad = 1;
        }
        if (bad) {
            if (torn)
                *torn = 1;
            break;
        }
        if (verify) {
            c = crc_buf(NONE, h, 12);
            if (crc_flash(fs, &c, r.addr + REC_LEN, r.nlen + r.len))
                return TFS_ERR_IO;
            if (~c != get32(h + 12))
                return TFS_ERR_CORRUPT;
        }
        if (cb && (res = cb(fs, &r, ctx)) != 0)
            return res;
        off += fp;
    }
    if (end)
        *end = off;
    return 0;
}

/* Walk every committed record of the log, oldest first. */
static int scan(tfs_t *fs, rec_cb cb, void *ctx)
{
    uint32_t i = fs->oldest;
    int res;
    for (;;) {
        if ((res = scan_sector(fs, i, cb, ctx, 0, 0, 0)) != 0)
            return res;
        if (i == fs->head)
            return 0;
        i = next_sec(fs, i);
    }
}

/* Make room for a record of footprint fp at the head. */
static int ensure_space(tfs_t *fs, uint32_t fp)
{
    const struct tfs_config *cfg = fs->cfg;
    uint32_t used = (fs->head + cfg->sector_count - fs->oldest) % cfg->sector_count + 1;
    uint32_t reserve = fp_of(fs, 0, 0);       /* room kept for a GC_DONE record */
    if (!fs->retired && fs->head_off + fp + reserve <= cfg->sector_size)
        return 0;
    if (used + 2 > cfg->sector_count)
        return TFS_ERR_NOSPC;
    return activate(fs, next_sec(fs, fs->head), NONE);
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

/* Find the live file called name: 1 = found (f->base is its base WRITE), 0 = none. */
static int find(tfs_t *fs, const char *name, struct find *f)
{
    int res;
    memset(f, 0, sizeof *f);
    if ((res = check_name(name, &f->nlen)) != 0)
        return res;
    f->name = name;
    if ((res = scan(fs, find_cb, f)) != 0)
        return res;
    return f->found;
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

static int do_mount(tfs_t *fs)
{
    const struct tfs_config *cfg = fs->cfg;
    uint32_t i, seq, gc_src, lo = NONE, hi = 0, prev = 0;
    int res, torn, any = 0;
    for (i = 0; i < cfg->sector_count; i++) {
        if ((res = read_hdr(fs, i, &seq, &gc_src)) < 0)
            return res;
        if (!res)
            continue;
        any = 1;
        if (seq < lo) {
            lo = seq;
            fs->oldest = (uint16_t)i;
        }
        if (seq >= hi) {
            hi = seq;
            fs->head = (uint16_t)i;
        }
    }
    if (!any)
        return TFS_ERR_CORRUPT;
    fs->sector_seq = hi;
    fs->next_seq = 1;
    /* the log must run oldest..head with rising sequence numbers */
    for (i = fs->oldest;; i = next_sec(fs, i)) {
        if (read_hdr(fs, i, &seq, &gc_src) != 1 || seq <= prev)
            return TFS_ERR_CORRUPT;
        prev = seq;
        if ((res = scan_sector(fs, i, max_seq_cb, &fs->next_seq, &fs->head_off, &torn, 1)) != 0)
            return res;
        if (i == fs->head)
            break;
    }
    fs->retired = (uint8_t)torn;
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
    int res;
    lock(fs);
    res = find(fs, name, &f);
    if (res == 0)
        res = TFS_ERR_NOENT;
    else if (res > 0)
        res = (int)f.base.len;
    return unlock(fs, res);
}

static int do_read(tfs_t *fs, const char *name, uint32_t off, void *buf, uint32_t len)
{
    struct find f;
    int res = find(fs, name, &f);
    if (res <= 0)
        return res ? res : TFS_ERR_NOENT;
    if (off >= f.base.len)
        return 0;
    if (len > f.base.len - off)
        len = f.base.len - off;
    if (rd(fs, f.base.addr + REC_LEN + f.base.nlen + off, buf, len))
        return TFS_ERR_IO;
    return (int)len;
}

int tfs_read(tfs_t *fs, const char *name, uint32_t off, void *buf, uint32_t len)
{
    lock(fs);
    return unlock(fs, do_read(fs, name, off, buf, len));
}

static int do_write(tfs_t *fs, const char *name, const void *buf, uint32_t len)
{
    struct find f;
    struct src s;
    uint8_t live[TFS_MAX_FILES / 8];
    uint16_t id;
    int res = find(fs, name, &f);
    if (res < 0)
        return res;
    if (!buf && len)
        return TFS_ERR_INVAL;
    if (len > (uint32_t)tfs_max_file_size(fs->cfg))
        return TFS_ERR_FBIG;
    if (res) {
        id = f.base.id;
    } else {
        if ((res = live_ids(fs, live)) != 0)
            return res;
        for (id = 0; id < TFS_MAX_FILES && (live[id >> 3] >> (id & 7) & 1); id++)
            ;
        if (id == TFS_MAX_FILES)
            return TFS_ERR_NOFILES;
    }
    if ((res = ensure_space(fs, fp_of(fs, f.nlen, len))) != 0)
        return res;
    s.data = buf;
    s.len = len;
    return put_record(fs, K_WRITE, id, name, f.nlen, &s);
}

int tfs_write(tfs_t *fs, const char *name, const void *buf, uint32_t len)
{
    lock(fs);
    return unlock(fs, do_write(fs, name, buf, len));
}
