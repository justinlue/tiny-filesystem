"""Build helper for tiny-filesystem.

    python tools/build.py test   build and run the host test suite
    python tools/build.py size   cross-compile src/tfs.c for Cortex-M and check budgets

Uses `python -m ziglang cc` (pip install ziglang) as the C compiler.
"""
import glob
import os
import struct
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BUILD = os.path.join(ROOT, "build")
CC = [sys.executable, "-m", "ziglang", "cc"]

TEXT_BUDGET = 4096
RAM_BUDGET = 256
CPUS = ["cortex_m0plus", "cortex_m4"]


def run(cmd):
    print(" ".join(os.path.basename(c) if os.path.isabs(c) else c for c in cmd))
    subprocess.run(cmd, check=True, cwd=ROOT)


def test():
    os.makedirs(BUILD, exist_ok=True)
    exe = os.path.join(BUILD, "tests.exe")
    srcs = sorted(glob.glob(os.path.join(ROOT, "test", "*.c")))
    tfs = os.path.join(ROOT, "src", "tfs.c")
    if os.path.exists(tfs):
        srcs.append(tfs)
    if os.environ.get("TFS_CC"):     # fallback compiler, e.g. tcc: no sanitizers
        cmd = [os.environ["TFS_CC"], "-Wall", "-Werror", "-g"]
    else:
        cmd = CC + ["-std=c99", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
                    "-fsanitize=undefined", "-fsanitize-trap=undefined"]
    run(cmd + ["-I", os.path.join(ROOT, "include"), "-I", os.path.join(ROOT, "test"),
               "-o", exe] + srcs)
    return subprocess.run([exe], cwd=ROOT).returncode


def elf_sizes(path):
    """Sum allocated section sizes of an ELF32 object into (text, ram)."""
    data = open(path, "rb").read()
    shoff, = struct.unpack_from("<I", data, 0x20)
    shentsize, shnum = struct.unpack_from("<HH", data, 0x2E)
    text = ram = 0
    for i in range(shnum):
        _, sh_type, flags, _, _, size = struct.unpack_from("<IIIIII", data, shoff + i * shentsize)
        if not flags & 0x2:          # SHF_ALLOC
            continue
        if sh_type == 0x70000001:    # SHT_ARM_EXIDX: clang emits unwind entries for C
            continue                 # (zig drops -fno-unwind-tables); GCC builds have none
        if flags & 0x4:              # SHF_EXECINSTR
            text += size
        elif flags & 0x1 or sh_type == 8:  # SHF_WRITE or SHT_NOBITS
            ram += size
        else:                        # read-only data lives in flash too
            text += size
    return text, ram


def size():
    os.makedirs(BUILD, exist_ok=True)
    ok = True
    for cpu in CPUS:
        obj = os.path.join(BUILD, f"tfs_{cpu}.o")
        run(CC + ["-target", "thumb-freestanding-eabi", f"-mcpu={cpu}", "-std=c99", "-Os",
                  "-ffunction-sections", "-fdata-sections", "-Wall", "-Wextra", "-Werror",
                  "-I", os.path.join(ROOT, "include"),
                  "-isystem", os.path.join(ROOT, "tools", "freestanding"),
                  "-c", os.path.join(ROOT, "src", "tfs.c"), "-o", obj])
        text, ram = elf_sizes(obj)
        verdict = "ok" if text < TEXT_BUDGET and ram < RAM_BUDGET else "OVER BUDGET"
        print(f"{cpu:14} flash {text:5} B (< {TEXT_BUDGET})  ram {ram:4} B (< {RAM_BUDGET})  {verdict}")
        ok &= verdict == "ok"
    return 0 if ok else 1


if __name__ == "__main__":
    cmds = {"test": test, "size": size}
    if len(sys.argv) != 2 or sys.argv[1] not in cmds:
        sys.exit(__doc__)
    sys.exit(cmds[sys.argv[1]]())
