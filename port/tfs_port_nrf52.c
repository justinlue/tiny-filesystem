/* tiny-filesystem port for nRF52 internal flash via nrfx_nvmc.
 *
 * UNTESTED REFERENCE PORT: written against the nrfx API but never compiled or
 * run on hardware. Verify on your board before relying on it.
 *
 * - Page (erase unit) is 4096 bytes; programming is per 32-bit word, which the
 *   nRF52 allows at most twice per erase (tfs programs each word once).
 * - Flash is memory-mapped, so read is a memcpy.
 * - With a SoftDevice enabled, NVMC access is forbidden: replace the calls with
 *   sd_flash_write() / sd_flash_page_erase() and block until the
 *   NRF_EVT_FLASH_OPERATION_SUCCESS (or _ERROR) SoC event arrives.
 * - Reserve the region in your linker script so the application never lands in it.
 *
 * Usage:
 *     struct tfs_config cfg;
 *     tfs_t fs;
 *     tfs_port_nrf52_config(&cfg, 0x000F0000, 16);   // 64 KB at 0xF0000
 *     if (tfs_mount(&fs, &cfg) < 0) { tfs_format(&fs, &cfg); tfs_mount(&fs, &cfg); }
 */
#include <string.h>
#include "nrfx_nvmc.h"
#include "tfs.h"

static int nrf_read(void *ctx, uint32_t addr, void *buf, uint32_t len)
{
    (void)ctx;
    memcpy(buf, (const void *)addr, len);
    return 0;
}

static int nrf_prog(void *ctx, uint32_t addr, const void *buf, uint32_t len)
{
    const uint8_t *p = buf;
    uint32_t word;
    (void)ctx;
    for (; len >= 4; len -= 4, addr += 4, p += 4) {
        memcpy(&word, p, 4);                 /* buf need not be word aligned */
        nrfx_nvmc_word_write(addr, word);
        while (!nrfx_nvmc_write_done_check())
            ;
    }
    return 0;
}

static int nrf_erase(void *ctx, uint32_t addr)
{
    (void)ctx;
    return nrfx_nvmc_page_erase(addr) == NRFX_SUCCESS ? 0 : -1;
}

void tfs_port_nrf52_config(struct tfs_config *cfg, uint32_t base, uint32_t pages)
{
    memset(cfg, 0, sizeof *cfg);
    cfg->read = nrf_read;
    cfg->prog = nrf_prog;
    cfg->erase = nrf_erase;
    cfg->base = base;
    cfg->sector_size = 4096;
    cfg->sector_count = pages;
    cfg->prog_size = 4;
}
