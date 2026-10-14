/* RAM-backed NOR flash simulator with strict program rules and power-cut injection.
 *
 * Rules enforced (a violating call returns SIM_ERR_RULE and bumps `violations`):
 *   - accesses must be in range; program address/length must be prog_size aligned;
 *   - erase address must be sector aligned;
 *   - a program unit may be programmed only once between erases;
 *   - programming can only clear bits (1 -> 0).
 *
 * Power cut: sim_cut_after(s, n) lets n program/erase operations succeed. The next one
 * is interrupted: a program leaves a random subset of the bits it would clear cleared;
 * an erase leaves the sector partially erased. From then on every call returns
 * SIM_ERR_CUT until sim_power_on().
 */
#ifndef SIM_FLASH_H
#define SIM_FLASH_H

#include <stdint.h>
#include "tfs.h"

#define SIM_ERR_CUT  (-1000)
#define SIM_ERR_RULE (-1001)
#define SIM_BASE     0x10000u   /* non-zero so base-offset bugs show up */

struct sim_flash {
    uint8_t *mem;
    uint8_t *programmed;        /* one flag per program unit */
    uint32_t sector_size, sector_count, prog_size;
    long ops_left;              /* ops before the cut; < 0 = never */
    int powered;
    uint32_t rng;
    long ops;                   /* total program + erase ops performed */
    long erases;                /* erase ops performed */
    int violations;
};

void sim_init(struct sim_flash *s, uint32_t sector_size, uint32_t sector_count, uint32_t prog_size);
void sim_free(struct sim_flash *s);
void sim_config(struct sim_flash *s, struct tfs_config *cfg);
void sim_cut_after(struct sim_flash *s, long n, uint32_t seed);
void sim_power_on(struct sim_flash *s);

int sim_read(void *ctx, uint32_t addr, void *buf, uint32_t len);
int sim_prog(void *ctx, uint32_t addr, const void *buf, uint32_t len);
int sim_erase(void *ctx, uint32_t addr);

#endif
