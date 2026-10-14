# tiny-filesystem (tfs) — Design

A deliberately tiny, power-loss-safe flash file system for Nordic (nRF52/53/54) and
TI (CC13xx/CC26xx/CC23xx) internal flash, also usable on external SPI NOR.

Status: design agreed (grilling session 2026-10-14). No code yet.

## Goals and budgets

| Item | Target |
|---|---|
| Code size (`.text`, `-Os`, Cortex-M0+/M4) | < 4 KB |
| Static RAM per mounted FS (`tfs_t`) | < 256 B (expected ~24 B) |
| Heap | none — caller supplies all buffers |
| Language | C99, depends only on `memcpy`/`memset`/`memcmp` |
| Region | 2..N erase units (typically 16–256 KB) |
| Files | ≤ 64, flat namespace |
| File size | ≤ one erase unit minus overhead (`tfs_max_file_size()`) |

Non-goals (v1): directories, random-access overwrite, files spanning sectors,
RAM index, wear statistics, encryption, sequence-number wraparound.

## Flash model (strictest common denominator)

- Erase unit (`sector_size`): power of two, e.g. 4 KB (nRF52) or 8 KB (CC26xx).
- Program unit (`prog_size`): power of two, 1..16 bytes; every program call is
  `prog_size`-aligned and a multiple of it.
- **Each program unit is programmed at most once between erases.** Programming only
  clears bits (1→0). Erased state is `0xFF`.
- Power can fail at any instant: an in-flight program may leave any subset of its
  bits cleared; an in-flight erase may leave a sector partially erased.

## Guarantees

- `tfs_mount` always succeeds on a region formatted by `tfs_format`, after any
  power cut at any point.
- Per-file atomicity: after a cut during `tfs_write`/`tfs_append`/`tfs_remove`, the
  file is either entirely its old version or entirely its new version.
- `tfs_mount` never writes and never auto-formats.

## Public API (`include/tfs.h`)

```c
struct tfs_config {
    void *ctx;
    int  (*read) (void *ctx, uint32_t addr, void *buf, uint32_t len);
    int  (*prog) (void *ctx, uint32_t addr, const void *buf, uint32_t len);
    int  (*erase)(void *ctx, uint32_t addr);          /* one sector */
    void (*lock)  (void *ctx);                        /* optional, may be NULL */
    void (*unlock)(void *ctx);                        /* optional, may be NULL */
    uint32_t base;          /* address of first sector */
    uint32_t sector_size;
    uint32_t sector_count;  /* >= 2 */
    uint32_t prog_size;     /* 1,2,4,8,16 */
};

typedef struct tfs { /* opaque-ish: cfg ptr + head sector/offset + next seq ... */ } tfs_t;
typedef struct { uint8_t next_id; } tfs_dir_t;
struct tfs_info { char name[16]; uint32_t size; };

int tfs_format (tfs_t *fs, const struct tfs_config *cfg);
int tfs_mount  (tfs_t *fs, const struct tfs_config *cfg);
int tfs_size   (tfs_t *fs, const char *name);                       /* >= 0: bytes */
int tfs_read   (tfs_t *fs, const char *name, uint32_t off,
                void *buf, uint32_t len);                           /* >= 0: bytes read */
int tfs_write  (tfs_t *fs, const char *name, const void *buf, uint32_t len); /* replace */
int tfs_append (tfs_t *fs, const char *name, const void *buf, uint32_t len); /* creates if missing */
int tfs_remove (tfs_t *fs, const char *name);
int tfs_dir_open(tfs_t *fs, tfs_dir_t *it);
int tfs_dir_next(tfs_t *fs, tfs_dir_t *it, struct tfs_info *info); /* 1 = entry, 0 = end */
int tfs_gc     (tfs_t *fs);                                         /* optional idle-time GC */
int tfs_max_file_size(const struct tfs_config *cfg);
```

The user-facing names map 1:1: fmount→`tfs_mount`, fsize→`tfs_size`,
fread→`tfs_read`, fwrite→`tfs_write`/`tfs_append`, remove-file→`tfs_remove`,
dir→`tfs_dir_open`/`tfs_dir_next`. The `tfs_` prefix avoids clashing with `<stdio.h>`.

Errors (negative): `TFS_ERR_IO` (port returned error), `TFS_ERR_CORRUPT`
(no valid FS / geometry mismatch), `TFS_ERR_NOENT`, `TFS_ERR_NOSPC`,
`TFS_ERR_FBIG` (file would exceed `tfs_max_file_size`), `TFS_ERR_INVAL`,
`TFS_ERR_NAMETOOLONG`, `TFS_ERR_NOFILES` (all 64 ids in use).
`NOSPC`/`FBIG`/`INVAL`/`NAMETOOLONG`/`NOFILES` are detected before anything is
written.

Names: 1–15 bytes, any byte except NUL, case-sensitive, byte compare, no path
semantics. Zero-length files are allowed.

Every public call takes `lock`/`unlock` around its body when provided. The core is
synchronous; async drivers (nRF SoftDevice `sd_flash_*`) block on completion in the port.

## On-flash format

All multi-byte fields little-endian. `align(x)` = round up to `prog_size`.

### Sector header (at sector offset 0, `align(24)` bytes)

| Off | Size | Field |
|---|---|---|
| 0 | 4 | magic `"TFS1"` |
| 4 | 4 | `sector_seq` — monotonic per sector activation, starts at 1 |
| 8 | 4 | `sector_size` (geometry check) |
| 12 | 2 | `sector_count` (geometry check) |
| 14 | 1 | format version = 1 |
| 15 | 1 | `prog_size` (geometry check) |
| 16 | 4 | `gc_src_seq` — `sector_seq` of the sector this one is compacting, or `0xFFFFFFFF` |
| 20 | 4 | CRC-32 of bytes 0..19 |

A sector is **valid** if magic and CRC match and geometry equals the config.
Otherwise it is **dirty/free**: never read for content, erase-verified (and erased
if needed) before reuse.

### Record (after the header, packed, each starts `prog_size`-aligned)

| Off | Size | Field |
|---|---|---|
| 0 | 1 | kind: `0xA1` WRITE, `0xA2` APPEND, `0xA3` DELETE, `0xA4` GC_DONE |
| 1 | 1 | `name_len` (WRITE only, else 0) |
| 2 | 2 | file id 0..63 |
| 4 | 4 | `seq` — global record sequence number |
| 8 | 4 | `len` — payload bytes |
| 12 | 4 | CRC-32 over bytes 0..11 + name + payload |
| 16 | n | name (WRITE only) |
| .. | len | payload |
| .. | pad | to `prog_size`; padding bytes left `0xFF` |
| .. | `prog_size` | **commit unit**: `0x3CA5C35A` repeated, programmed last |

Record footprint = `align(16 + name_len + len) + prog_size`.
A record is **committed** if the commit unit matches exactly and the CRC verifies.
Records never span sectors.
`tfs_max_file_size` = largest `len` such that a WRITE with a 15-byte name fits in an
empty sector after the header.

### Scanning a sector

Walk records from the header. If a header unit is all `0xFF`, that's the end of data
(free space starts here). If a record isn't committed, the rest of the sector is
**retired**: nothing after it is read, and nothing more is written to this sector.

## Semantics

- Log order = valid sectors sorted by `sector_seq`, then physical offset.
- A file is identified by its id. Its **base** is the newest WRITE or DELETE record
  for that id. If the base is a DELETE, or there is no base, the file doesn't exist.
  Because GC only copies live records, the newest WRITE/DELETE for an id is always
  also the last one in log order. That lets a single forward pass build the 64-bit
  live-id bitmap.
- **Name lookup:** the file named N is the id whose base is a WRITE carrying name N.
- **Content:** the base payload followed by every APPEND for that id with
  `seq > base.seq`, in `seq` order. (GC copies keep their original `seq`, so append
  order is by `seq`, not by physical position.)
- `tfs_write` writes a new WRITE record. The old one becomes dead the moment the new
  one commits.
- `tfs_append` writes an APPEND record. It creates the file if it's missing (a WRITE
  of the data). If the chain already has **8** appends, it writes a merged WRITE
  record of old content plus new data instead, through the normal write path.
- `tfs_remove` writes a DELETE record.
- **Id allocation:** the lowest id not live (one scan). If all 64 are live, return `NOFILES`.
- `next_seq` is the highest record `seq` seen at mount plus 1, then increments.
  `0xFFFFFFFF` is never used.

## Space management and GC

Invariant: **at least one valid-free (erased or dirty) sector exists after any
completed operation.**

Advancing the head (the current record won't fit, or the head is retired):
1. If free sectors ≥ 2: activate one (erase-verify, write a header with
   `gc_src_seq = 0xFFFFFFFF`), then write the record there.
2. If free sectors == 1: **GC** the oldest valid sector `O` into the free sector `D`:
   1. Write `D`'s header with `gc_src_seq = O.sector_seq`.
   2. Copy every *live* record of `O` into `D` byte-for-byte (same kind, id, `seq`, payload).
      - WRITE is live if it's its id's base.
      - APPEND is live if its id's base is a WRITE with lower `seq`.
      - DELETE and GC_DONE are dropped. Every older record for that id is in `O` or
        already gone.
   3. Write a GC_DONE record in `D`.
   4. Erase `O`.
   5. Retry the original write. Repeat GC, at most `sector_count` times, until it
      fits; otherwise return `NOSPC`.
   Since `O`'s live records came from one sector, they always fit in `D`.
3. Pre-check: if the total footprint of live records plus the new record exceeds
   `(sector_count − 1) × (sector_size − header)`, return `NOSPC` before writing anything.

Mount-time GC recovery (read-only, decided in RAM):
- A valid sector whose `gc_src_seq` names a source but has **no GC_DONE** was an
  interrupted GC. It's treated as dirty: ignored, and erased on reuse. The source
  is still intact and authoritative.
- A valid sector **with GC_DONE**: the sector whose `sector_seq == gc_src_seq` is
  obsolete and treated as dirty regardless of what it contains. This covers a
  half-finished erase of `O`.
- No valid sectors, or a geometry mismatch: `CORRUPT`.

`tfs_gc()` runs one GC step if there's a sector worth compacting. It's meant to be
called during idle time so writes are less likely to pay for GC.

`tfs_format`: erase all sectors, write sector 0's header with `sector_seq = 1`.
Rejects `sector_count < 2` or a bad `prog_size`/`sector_size`.

## RAM

`tfs_t`: config pointer, head sector index, head write offset, head-retired flag,
`next_seq`, max `sector_seq`. About 24 B. Scans use a small stack buffer, at most
32 B plus `prog_size`. `tfs_dir_t` is 1 byte (the next id). Each `tfs_dir_next`
does about two scans.

## Testing

Host-built with `python -m ziglang cc` (installed by `pip install ziglang`).

- `test/sim_flash.c`: a RAM-backed simulated flash. Its rules:
  - It rejects programming a bit from 0 to 1, programming a unit twice,
    unaligned access, and out-of-range access.
  - A test can cut power before the N-th program or erase.
  - The cut program leaves a random subset of its bits cleared.
  - The cut erase leaves the sector partially erased.
- Unit tests cover the API, the error codes, the name rules, `FBIG`/`NOSPC`/
  `NOFILES`, and `dir`.
- The **power-cut suite** runs a scripted workload with a cut at every N. After each
  cut it remounts and checks that every file equals its last committed content or
  the one before. It then continues writing to prove the FS is still usable.
- A seeded random fuzz runs the same checks.
- All suites run with `prog_size` 4 and 16, and with 2 sectors and 8 sectors.
- `tools/build.py test` builds and runs the tests. `tools/build.py size`
  cross-compiles `src/tfs.c` for `cortex-m0plus` and `cortex-m4` at `-Os`. It fails
  if `.text` is 4096 B or more, or `.data` + `.bss` is 256 B or more.

## Layout

```
tiny-filesystem/
  include/tfs.h
  src/tfs.c
  port/tfs_port_nrf52.c      # nrfx_nvmc — UNTESTED reference port
  port/tfs_port_cc26xx.c     # driverlib FlashSectorErase/FlashProgram — UNTESTED reference port
  test/sim_flash.{c,h}
  test/test_*.c
  tools/build.py
  DESIGN.md  CLAUDE.md
```
Own git repo, developed test-first.
