# CLAUDE.md

tiny-filesystem (`tfs`): a tiny, power-loss-safe flash file system in C99 for nRF / TI
internal flash. `DESIGN.md` is the source of truth for the on-flash format, the GC and
mount algorithms, and the guarantees. Read it before changing `src/tfs.c`.

## Commands

- `python tools/build.py test` builds and runs every host test (zig cc + UBSan).
- `python tools/build.py size` cross-compiles for Cortex-M0+/M4 and enforces the
  < 4 KB flash and < 256 B RAM budgets.
- No zig installed: set `TFS_CC` to a C compiler (e.g. TinyCC) for `test`. That
  build has no sanitizer, and `size` still needs zig.

## Invariants the tests enforce

- **Write-once:** a program unit is programmed at most once between erases.
  `test/sim_flash.c` turns every breach into a `violations` count, and each test
  ends by asserting it is 0. A program cut by power loss can leave a unit that reads
  `0xFF` but is already used. That is why mount retires the head and activation
  always erases. Every new write path has to respect this too.
- **The power-cut suite is the arbiter** for any change to write, GC, or mount
  (`test/test_powercut.c`, which cuts at every program/erase). Keep it green at all
  geometries before claiming a change works.
- **Tests stay at the seams:** the `tfs.h` API, the simulator's contract, and the
  size gate. They never assert on raw on-flash bytes. The one exception is
  deliberate corruption in the bit-rot test.

## Ports

`port/` holds reference ports for nRF52 (nrfx_nvmc) and CC26xx (driverlib). They
have never been compiled or run on hardware. Say so whenever you rely on them.
