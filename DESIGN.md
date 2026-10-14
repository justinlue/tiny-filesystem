# tiny-filesystem (tfs) — Design

A deliberately tiny, power-loss-safe flash file system for Nordic (nRF52/53/54) and
TI (CC13xx/CC26xx/CC23xx) internal flash, also usable on external SPI NOR.

Status: v1 is implemented and the host tests pass. The design was agreed in a grilling
session on 2026-10-14. Points marked **(changed in implementation)** were forced by
tests and differ from that session.

## Goals and budgets

| Item | Target |
|---|---|
| Code size (`.text`, `-Os`, Cortex-M0+/M4) | < 4 KB |
| Static RAM per mounted FS (`tfs_t`) | < 256 B (actual: 28 B) |
| Heap | none — caller supplies all buffers |
| Language | C99, depends only on `memcpy`/`memset`/`memcmp`/`strlen` |
| Region | 2..65535 erase units (typically 16–256 KB) |
| Files | ≤ 64, flat namespace |
| File size | ≤ one erase unit minus overhead (`tfs_max_file_size()`) |

Non-goals (v1): directories, random-access overwrite, files spanning sectors,
RAM index, wear statistics, encryption, sequence-number wraparound.

## Flash model (strictest common denominator)

- Erase unit (`sector_size`): a power of two, e.g. 4 KB (nRF52) or 8 KB (CC26xx).
- Program unit (`prog_size`): a power of two from 1 to 16 bytes. Every program call is
  `prog_size`-aligned and a multiple of it.
- **Each program unit is programmed at most once between erases.** Programming only
  clears bits (1→0). Erased state is `0xFF`.
- Power can fail at any instant:
  - An interrupted program may leave any subset of its bits cleared, **including
    none at all**. That case reads as erased but must not be programmed again.
  - An interrupted erase may leave a sector partially erased.

## Guarantees

- `tfs_mount` always succeeds on a region formatted by `tfs_format`, after a power cut
  at any point.
- Per-file atomicity: after a cut during `tfs_write`, `tfs_append`, or `tfs_remove`,
  the file is either entirely its old version or entirely its new version.
- `tfs_mount` never writes and never auto-formats.
- After any `TFS_ERR_IO` (a port error), call `tfs_mount` again before further use.
- Bit rot in committed data is reported, not hidden: `tfs_mount` checks every
  record's CRC and returns `TFS_ERR_CORRUPT` on a mismatch.

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
int tfs_gc     (tfs_t *fs);                                         /* 1 = ran a GC, 0 = not needed */
int tfs_max_file_size(const struct tfs_config *cfg);
```

The user-facing names map 1:1:

| Requested | tfs function |
|---|---|
| fmount | `tfs_mount` |
| fsize | `tfs_size` |
| fread | `tfs_read` |
| fwrite | `tfs_write` / `tfs_append` |
| remove-file | `tfs_remove` |
| dir | `tfs_dir_open` / `tfs_dir_next` |

The `tfs_` prefix avoids clashing with `<stdio.h>`.

Errors are negative:

| Error | Meaning |
|---|---|
| `TFS_ERR_IO` | the port returned an error |
| `TFS_ERR_CORRUPT` | no valid FS, geometry mismatch, or bad CRC |
| `TFS_ERR_NOENT` | no such file |
| `TFS_ERR_NOSPC` | not enough free space |
| `TFS_ERR_FBIG` | the file would exceed `tfs_max_file_size` |
| `TFS_ERR_INVAL` | bad argument or configuration |
| `TFS_ERR_NAMETOOLONG` | name longer than 15 bytes |
| `TFS_ERR_NOFILES` | all 64 file ids are in use |

`FBIG`, `INVAL`, `NAMETOOLONG`, and `NOFILES` are detected before anything is written.
So is `NOSPC`, whenever live data plus the new record exceeds capacity (see
*Space management*).

Names are 1–15 bytes of anything except NUL. They are case-sensitive, compared byte
by byte, and have no path semantics. Zero-length files are allowed.

Every public call except `tfs_dir_open` and `tfs_max_file_size` runs between `lock` and `unlock` when they're
provided. The core is synchronous. Async drivers, such as the nRF SoftDevice's
`sd_flash_*`, should block on completion inside the port.

## On-flash format

All multi-byte fields are little-endian. `align(x)` means x rounded up to `prog_size`.

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

A sector is **valid** if magic and CRC match and the geometry equals the config.

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
| .. | pad | to `prog_size`, `0xFF` |
| .. | `prog_size` | **commit unit**: `5A C3 A5 3C` repeated, programmed last |

- Record footprint = `align(16 + name_len + len) + prog_size`.
- A record is **committed** if its header is sane and the commit unit matches
  exactly. The commit unit is programmed last, so a committed record was fully
  written. Its CRC therefore only guards against bit rot, and is checked at mount
  rather than on every scan.
- Records never span sectors.
- `tfs_max_file_size` is the largest `len` for which a WRITE with a 15-byte name fits
  in one sector's usable space (see *Space management*). That's 3997 bytes for 4 KB
  sectors at `prog_size` 4, and 925 bytes for 1 KB sectors.

Scanning a sector walks records from the header. An all-`0xFF` record header marks
the end of data. A record that isn't committed ends the walk: nothing after it in
that sector is read.

## Semantics

- **Log order** is sectors in `oldest..head` order, then physical offset.
- A file is identified by its id. Its **base** is the newest WRITE or DELETE record
  for that id. If the base is a DELETE, or there's no base, the file doesn't exist.
  GC only copies live records, so the newest WRITE/DELETE for an id is always also
  the last one in log order. That means a single forward pass can build the 64-bit
  live-id bitmap.
- **Name lookup:** the file named N is the id whose base is a WRITE carrying name N.
- **Content:** the base payload, followed by every APPEND for that id with
  `seq > base.seq`, in `seq` order. GC copies keep their original `seq`, so appends
  are ordered by `seq`, not by physical position.
- `tfs_write` writes a WRITE record. The old version is dead the moment the new one
  commits.
- `tfs_append` writes an APPEND record. If the file is missing, it creates it with a
  WRITE. If the chain already has **8** appends, it instead writes one merged WRITE
  holding the old content plus the new data.
- `tfs_remove` writes a DELETE record.
- **Id allocation:** the lowest id that isn't live, found in one scan. If none is
  free, the result is `NOFILES`.
- `next_seq` is the highest record `seq` seen at mount plus 1. `0xFFFFFFFF` is never
  used.

## Space management and GC

**Sector layout over time.** The log is a circular run of sectors from `oldest` to
`head`. The sector after the head is always free. Before the first GC, sectors are
used in order starting from 0. After it, exactly one sector is free.

**Reserves (changed in implementation).** Each sector keeps room at its end for two
records:
- **A GC_DONE record**, until the sector holds its own GC_DONE. This guarantees a GC
  into the sector always completes.
- **A DELETE record**, which every record except a DELETE must leave free. This
  guarantees a completely full FS can still remove a file. GC drops DELETE records,
  so a freshly compacted sector always has room for one.

`usable = sector_size − align(24) − 2 × (align(16) + prog_size)` is the space WRITE
and APPEND records may fill. Capacity is `(sector_count − 1) × usable`.

**Precheck.**
- `tfs_t.live` is the total footprint of all live records. It's computed at mount and
  kept up to date by each operation.
- A write or append whose new record would push `live` over capacity returns `NOSPC`
  before touching flash.
- The old version of a file being replaced is *not* subtracted, because it stays live
  until the new record commits. So a replace needs room for both versions. For
  example, a max-size file on a 2-sector FS can be written once but not rewritten.
- `tfs_remove` has no precheck.

**Limitation (changed in implementation).** Records don't span sectors, so a
sector's tail can be too small for the next record. The precheck doesn't model that
waste. Near a full FS, a write that passed the precheck can still fail with `NOSPC`,
after up to `sector_count` GC steps. Those steps never lose or change data, but they
do cost erases.

**Advancing the head.** This happens when the record won't fit or the head is
retired:
1. If two or more sectors are free, activate the next one.
2. If exactly one is free, **GC** the oldest sector `O` into it (call it `D`):
   1. Erase `D` and write its header with `gc_src_seq = O.sector_seq`.
   2. Copy every *live* record of `O` into `D` byte for byte, keeping kind, id, `seq`,
      payload, and CRC.
      - A WRITE is live if it's its id's base.
      - An APPEND is live if its id's base is a WRITE with a lower `seq`.
      - DELETE and GC_DONE records are dropped. Every older record for that id is in
        `O` or already gone.

      Liveness is worked out in batches of 16 ids: one log scan for their bases, then
      one scan of `O`.
   3. Write a GC_DONE record in `D`. `O` is now the free sector. **It isn't erased
      yet; it's erased when it's next activated (changed in implementation)**, which
      gives one erase per sector cycle.
3. Retry. After `sector_count` attempts, return `NOSPC`.

**Activation always erases (changed in implementation).** Flash that reads `0xFF` may
still hold an interrupted program. Checking that a sector reads erased is therefore
not enough to make it safe to program.

**Mount (changed in implementation).** Mount is read-only and works everything out
in RAM:
1. **Find the head.** It's the valid sector with the highest `sector_seq`. The
   exception is when that sector has a `gc_src_seq` but no committed GC_DONE. That
   was an interrupted GC target. It's ignored (it's the free sector after the real
   head), and the next-highest sector is the head.
2. **Find the oldest sector.** Walk back from the head while sectors are valid and
   `sector_seq` keeps falling, never stepping onto the sector after the head. The
   last sector reached is `oldest`. The walk never includes a GC source, whether it
   was erased, half-erased, or left untouched.
3. **Restore counters.** Scan the log once, checking every CRC, to get `next_seq`.
   Then compute `live`.
4. **Retire the head.** Flash after the head's data may hold a program that was
   interrupted without changing a bit, so writing resumes in a freshly erased sector.
   Each mount costs the unused tail of one sector, which GC reclaims later.

**`tfs_gc()`** runs a GC when the next head advance would need one and the head is
either retired or less than a quarter free. Call it during idle time so writes don't
pay for GC.

**`tfs_format`** erases every sector and activates sector 0 with `sector_seq = 1`. It
rejects:
- `sector_count < 2`
- a `prog_size` other than 1, 2, 4, 8, or 16
- a `sector_size` that isn't a power of two of at least 128

## RAM and speed

- `tfs_t` is 28 B on a 32-bit MCU: config pointer, oldest and head indices, head
  offset, `next_seq`, `sector_seq`, `live`, and two flags.
- Stack use is estimated to peak at about 200 B, during GC and mount (a batch of 16 bases plus a
  32 B read buffer).
- `tfs_dir_t` is 1 byte.
- Every lookup is a full log scan. `tfs_read` costs one scan per append record in the
  chain, at most 9 in total.

## Testing

The host build uses `python -m ziglang cc` (from `pip install ziglang`) with UBSan.
Without zig, `TFS_CC=path/to/tcc` builds the tests with TinyCC instead, without a
sanitizer.

- **`test/sim_flash.c`** is a RAM-backed simulated flash:
  - It rejects programming a bit from 0 to 1, programming a unit twice, unaligned
    access, and out-of-range access.
  - A test can cut power before the N-th program or erase.
  - An interrupted program leaves a random subset of its bits cleared.
  - An interrupted erase leaves junk that can't be programmed.
- **Unit tests** cover the API, error codes, name rules, `FBIG`, `NOSPC`, `NOFILES`,
  dir, GC churn, config checks, geometry mismatch, bit rot, and lock wrapping.
- **The power-cut suite** (`test/test_powercut.c`) runs a 120-operation workload of
  writes, appends, and removes against a reference model.
  - It cuts power at every possible program/erase, remounts, and checks every file
    against the model. The file being changed may hold its old or its new content.
  - It then finishes the workload, to prove the FS is still usable.
  - A seeded fuzz adds random workloads with repeated cuts.
- **Geometries tested:** 4×512 B (prog 4), 2×1 KB (prog 4), 4×512 B (prog 16), and
  8×256 B (prog 1). The fuzz also runs at prog 8 and 16.
- **`tools/build.py size`** cross-compiles `src/tfs.c` for `cortex-m0plus` and
  `cortex-m4` at `-Os`. It fails if `.text` reaches 4096 B or `.data` + `.bss`
  reaches 256 B.

## Layout

```
tiny-filesystem/
  include/tfs.h
  src/tfs.c
  port/tfs_port_nrf52.c      # nrfx_nvmc — UNTESTED, uncompiled reference port
  port/tfs_port_cc26xx.c     # driverlib — UNTESTED, uncompiled reference port
  test/sim_flash.{c,h}  test/check.h  test/test_*.c
  tools/build.py
  DESIGN.md  CLAUDE.md
```
