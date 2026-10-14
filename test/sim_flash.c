#include <stdlib.h>
#include <string.h>
#include "sim_flash.h"

static uint32_t rnd(struct sim_flash *s)
{
    s->rng ^= s->rng << 13;
    s->rng ^= s->rng >> 17;
    s->rng ^= s->rng << 5;
    return s->rng;
}

static int violation(struct sim_flash *s)
{
    s->violations++;
    return SIM_ERR_RULE;
}

void sim_init(struct sim_flash *s, uint32_t sector_size, uint32_t sector_count, uint32_t prog_size)
{
    uint32_t size = sector_size * sector_count;
    memset(s, 0, sizeof *s);
    s->sector_size = sector_size;
    s->sector_count = sector_count;
    s->prog_size = prog_size;
    s->mem = malloc(size);
    s->programmed = calloc(size / prog_size, 1);
    memset(s->mem, 0xFF, size);
    s->ops_left = -1;
    s->powered = 1;
    s->rng = 1;
}

void sim_free(struct sim_flash *s)
{
    free(s->mem);
    free(s->programmed);
}

void sim_config(struct sim_flash *s, struct tfs_config *cfg)
{
    memset(cfg, 0, sizeof *cfg);
    cfg->ctx = s;
    cfg->read = sim_read;
    cfg->prog = sim_prog;
    cfg->erase = sim_erase;
    cfg->base = SIM_BASE;
    cfg->sector_size = s->sector_size;
    cfg->sector_count = s->sector_count;
    cfg->prog_size = s->prog_size;
}

void sim_cut_after(struct sim_flash *s, long n, uint32_t seed)
{
    s->ops_left = n;
    s->rng = seed * 2654435761u + 1;
}

void sim_power_on(struct sim_flash *s)
{
    s->powered = 1;
    s->ops_left = -1;
}

static int in_range(struct sim_flash *s, uint32_t addr, uint32_t len)
{
    uint32_t size = s->sector_size * s->sector_count;
    return addr >= SIM_BASE && addr - SIM_BASE <= size && len <= size - (addr - SIM_BASE);
}

/* Returns 1 if this operation is the one interrupted by the power cut. */
static int cut_now(struct sim_flash *s)
{
    s->ops++;
    if (s->ops_left < 0)
        return 0;
    if (s->ops_left-- > 0)
        return 0;
    s->powered = 0;
    return 1;
}

int sim_read(void *ctx, uint32_t addr, void *buf, uint32_t len)
{
    struct sim_flash *s = ctx;
    if (!s->powered)
        return SIM_ERR_CUT;
    if (!in_range(s, addr, len))
        return violation(s);
    memcpy(buf, s->mem + (addr - SIM_BASE), len);
    return 0;
}

int sim_prog(void *ctx, uint32_t addr, const void *buf, uint32_t len)
{
    struct sim_flash *s = ctx;
    const uint8_t *src = buf;
    uint32_t off = addr - SIM_BASE, i;
    if (!s->powered)
        return SIM_ERR_CUT;
    if (!in_range(s, addr, len) || off % s->prog_size || len % s->prog_size)
        return violation(s);
    for (i = 0; i < len; i++)
        if ((~s->mem[off + i] & src[i]) || s->programmed[(off + i) / s->prog_size])
            return violation(s);
    if (cut_now(s)) {   /* a random subset of the bits being cleared get cleared */
        for (i = 0; i < len; i++)
            s->mem[off + i] &= ~((uint8_t)~src[i] & (uint8_t)rnd(s));
    } else {
        for (i = 0; i < len; i++)
            s->mem[off + i] &= src[i];
    }
    for (i = 0; i < len; i += s->prog_size)
        s->programmed[(off + i) / s->prog_size] = 1;
    return s->powered ? 0 : SIM_ERR_CUT;
}

int sim_erase(void *ctx, uint32_t addr)
{
    struct sim_flash *s = ctx;
    uint32_t off = addr - SIM_BASE, u, i, p = s->prog_size;
    if (!s->powered)
        return SIM_ERR_CUT;
    if (!in_range(s, addr, s->sector_size) || off % s->sector_size)
        return violation(s);
    if (!cut_now(s)) {
        memset(s->mem + off, 0xFF, s->sector_size);
        memset(s->programmed + off / p, 0, s->sector_size / p);
        return 0;
    }
    /* Partial erase: each unit ends up either truly erased or holding junk (some
     * bits set back to 1, at least one bit still 0) that must not be programmed. */
    for (u = off; u < off + s->sector_size; u += p) {
        int erased = rnd(s) & 1, junk = 0;
        for (i = 0; i < p; i++) {
            s->mem[u + i] = erased ? 0xFF : s->mem[u + i] | (uint8_t)rnd(s);
            junk |= s->mem[u + i] != 0xFF;
        }
        s->programmed[u / p] = (uint8_t)junk;
    }
    return SIM_ERR_CUT;
}
