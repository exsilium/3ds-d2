#!/usr/bin/env python3
"""Decodes a Luma3DS ARM11 crash dump (SD card: luma/dumps/arm11/*.dmp) and
prints the registers. With an ELF, also the source line of the PC, the LR and
every stack word that points into the app's code (via arm-none-eabi-addr2line,
run in the toolchain image).

    python3 -I tools/luma_dump.py <dump.dmp> [build/cpu/ctr/3ds-d2-game-cia.elf]

Layout per Luma3DS's exception dump header (public): two magic words, version,
processor/core, exception type, total size, then the sizes of the register,
code, stack and additional-data sections, which follow in that order.
"""
import os
import struct
import subprocess
import sys

REGS = ["r0", "r1", "r2", "r3", "r4", "r5", "r6", "r7", "r8", "r9", "r10", "r11", "r12",
        "sp", "lr", "pc", "cpsr", "dfsr", "ifsr", "far", "fpexc", "fpinst", "fpinst2"]
TYPES = {0: "FIQ", 1: "undefined instruction", 2: "prefetch abort", 3: "data abort"}
CODE_LO, CODE_HI = 0x00100000, 0x00600000   # the app's .text/.rodata range


def symbolize(elf, addrs):
    if not elf or not addrs:
        return {}
    root = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
    rel = os.path.relpath(os.path.abspath(elf), root)
    cmd = ["docker", "run", "--rm", "-v", f"{root}:/work", "-w", "/work", "3ds-d2/toolchain:local",
           "arm-none-eabi-addr2line", "-f", "-C", "-e", rel] + ["0x%08x" % a for a in addrs]
    out = subprocess.run(cmd, capture_output=True, text=True).stdout.splitlines()
    return {a: (out[2 * i], out[2 * i + 1]) for i, a in enumerate(addrs) if 2 * i + 1 < len(out)}


def main(path, elf=None):
    data = open(path, "rb").read()
    m1, m2, vmin, vmaj, proc, core, etype, total, rsize, csize, ssize, asize = struct.unpack_from("<IIHHHHIIIIII", data, 0)
    hdr = 0x28
    if (m1, m2) != (0xDEADC0DE, 0xDEADCAFE):
        print("warning: unexpected magic %08x %08x" % (m1, m2))
    print(f"Luma dump v{vmaj}.{vmin}  processor ARM{proc} core {core}  exception: {TYPES.get(etype, etype)}")
    regs = struct.unpack_from("<%dI" % (rsize // 4), data, hdr)
    for name, val in zip(REGS, regs):
        print(f"  {name:>7} = {val:08x}")
    stack_off = hdr + rsize + csize
    stack = struct.unpack_from("<%dI" % (ssize // 4), data, stack_off)
    extra = data[stack_off + ssize:stack_off + ssize + asize]
    name = extra[:8].split(b"\0")[0].decode("ascii", "replace")
    if len(extra) >= 16:
        print(f"  process {name}  title {struct.unpack_from('<Q', extra, 8)[0]:016x}")
    reg = dict(zip(REGS, regs))
    picks = [reg.get("pc", 0), reg.get("lr", 0)] + [w for w in stack if CODE_LO <= w < CODE_HI]
    seen, uniq = set(), []
    for a in picks:
        if a not in seen:
            seen.add(a)
            uniq.append(a)
    syms = symbolize(elf, uniq[:80])
    if syms:
        print("code addresses (pc, lr, then stack words):")
        for a in uniq[:80]:
            fn, line = syms.get(a, ("?", "?"))
            if fn != "??":
                print(f"  {a:08x}  {fn}  {line.replace('./tests/cpu/ctr/../../../', '')}")


if __name__ == "__main__":
    main(sys.argv[1], sys.argv[2] if len(sys.argv) > 2 else None)
