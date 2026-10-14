/* tiny-filesystem port for TI CC13xx/CC26xx internal flash via driverlib.
 *
 * UNTESTED REFERENCE PORT: written against the driverlib flash API but never
 * compiled or run on hardware. Verify on your board before relying on it.
 *
 * - Sector (erase unit) is 8 KB on CC13x2/CC26x2 (4 KB on CC26x0); the code
 *   asks FlashSectorSizeGet().
 * - The flash controller limits how many program operations a row may take
 *   between erases (see your device's TRM). tfs issues one program call per
 *   prog_size unit, so this port uses prog_size 16 to keep that count low;
 *   check the limit for your part.
 * - driverlib requires the VIMS cache to be disabled around program/erase and
 *   the flash must not be read (no code executing from it) meanwhile; the
 *   functions run from ROM. If you use TI-RTOS / SimpleLink, prefer wrapping
 *   the NVS driver instead, which handles both.
 * - Never place the region over the CCFG page (the last sector).
 *
 * Usage:
 *     tfs_port_cc26xx_config(&cfg, 0x00050000, 4);   // 32 KB at 0x50000
 */
#include <string.h>
#include <ti/devices/DeviceFamily.h>
#include DeviceFamily_constructPath(driverlib/flash.h)
#include DeviceFamily_constructPath(driverlib/vims.h)
#include "tfs.h"

static int cc_read(void *ctx, uint32_t addr, void *buf, uint32_t len)
{
    (void)ctx;
    memcpy(buf, (const void *)addr, len);
    return 0;
}

static uint32_t cache_off(void)
{
    uint32_t mode = VIMSModeGet(VIMS_BASE);
    while (mode == VIMS_MODE_CHANGING)
        mode = VIMSModeGet(VIMS_BASE);
    if (mode != VIMS_MODE_DISABLED) {
        VIMSModeSet(VIMS_BASE, VIMS_MODE_DISABLED);
        while (VIMSModeGet(VIMS_BASE) != VIMS_MODE_DISABLED)
            ;
    }
    return mode;
}

static void cache_restore(uint32_t mode)
{
    if (mode != VIMS_MODE_DISABLED)
        VIMSModeSet(VIMS_BASE, mode);
}

static int cc_prog(void *ctx, uint32_t addr, const void *buf, uint32_t len)
{
    uint32_t mode = cache_off(), st;
    (void)ctx;
    st = FlashProgram((uint8_t *)buf, addr, len);
    cache_restore(mode);
    return st == FAPI_STATUS_SUCCESS ? 0 : -1;
}

static int cc_erase(void *ctx, uint32_t addr)
{
    uint32_t mode = cache_off(), st;
    (void)ctx;
    st = FlashSectorErase(addr);
    cache_restore(mode);
    return st == FAPI_STATUS_SUCCESS ? 0 : -1;
}

void tfs_port_cc26xx_config(struct tfs_config *cfg, uint32_t base, uint32_t sectors)
{
    memset(cfg, 0, sizeof *cfg);
    cfg->read = cc_read;
    cfg->prog = cc_prog;
    cfg->erase = cc_erase;
    cfg->base = base;
    cfg->sector_size = FlashSectorSizeGet();
    cfg->sector_count = sectors;
    cfg->prog_size = 16;
}
