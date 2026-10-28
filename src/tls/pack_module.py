#!/usr/bin/env python3
"""The TLS module's ELF (linked by module.ld with --emit-relocs) as tls.bin:
the header of tls_module.h, the image from address 0, and the offsets of the
words that hold an absolute address, which the loader adds its load address
to. Usage: pack_module.py <module.elf> <tls.bin>

Only the relocation kinds a static, non-PIC ARM link leaves are understood:
absolute words (R_ARM_ABS32, R_ARM_TARGET1) are listed; PC-relative ones
need nothing, as the whole image moves together. Anything else stops the
build, as would constructors (the loader runs none)."""
import struct
import sys

MAGIC = 0x444D4C54   # "TLMD"
ABI = 1              # TLS_MODULE_ABI

ABSOLUTE = {2, 38}                                      # ABS32, TARGET1
RELATIVE = {0, 1, 3, 10, 28, 29, 30, 40, 42, 102, 103}  # NONE, PC24, REL32, THM_CALL, CALL, JUMP24,
                                                        # THM_JUMP24, V4BX, PREL31, THM_JUMP11, THM_JUMP8
SHF_ALLOC, SHT_NOBITS, SHT_REL, SHT_RELA, SHT_SYMTAB = 2, 8, 9, 4, 2
SHN_UNDEF, SHN_ABS = 0, 0xFFF1


def fail(msg):
    sys.exit("pack_module.py: " + msg)


def main(src, dst):
    elf = open(src, "rb").read()
    if elf[:4] != b"\x7fELF" or elf[4] != 1 or elf[5] != 1:
        fail("not a 32-bit little-endian ELF")
    e_shoff, = struct.unpack_from("<I", elf, 32)
    e_shentsize, e_shnum, e_shstrndx = struct.unpack_from("<HHH", elf, 46)
    secs = [struct.unpack_from("<10I", elf, e_shoff + i * e_shentsize) for i in range(e_shnum)]
    shstr = secs[e_shstrndx]

    def name(s):
        o = shstr[4] + s[0]
        return elf[o:elf.index(b"\0", o)].decode()

    alloc = [s for s in secs if s[2] & SHF_ALLOC and s[5]]
    image_end = max((s[3] + s[5] for s in alloc if s[1] != SHT_NOBITS), default=0)
    mem_end = max((s[3] + s[5] for s in alloc), default=0)
    image = bytearray(image_end)
    for s in alloc:
        if name(s) in (".init_array", ".ctors"):
            fail("the module has constructors (%s)" % name(s))
        if s[1] != SHT_NOBITS:
            image[s[3]:s[3] + s[5]] = elf[s[4]:s[4] + s[5]]

    symtab = next((s for s in secs if s[1] == SHT_SYMTAB), None)
    if not symtab:
        fail("no symbol table")
    strtab = secs[symtab[6]]

    def symbol(i):
        st_name, value, size, info, other, shndx = struct.unpack_from("<IIIBBH", elf, symtab[4] + i * 16)
        o = strtab[4] + st_name
        return elf[o:elf.index(b"\0", o)].decode(), value, shndx

    entry = next((v for i in range(symtab[5] // 16) for n, v, x in [symbol(i)] if n == "tls_module_entry"), None)
    if entry is None:
        fail("no tls_module_entry")

    relocs = set()
    for s in secs:
        if s[1] == SHT_RELA:
            fail("RELA relocations in " + name(s))
        if s[1] != SHT_REL:
            continue
        target = secs[s[7]]
        if not target[2] & SHF_ALLOC:
            continue
        for i in range(s[5] // 8):
            r_offset, r_info = struct.unpack_from("<II", elf, s[4] + i * 8)
            kind, sym = r_info & 0xFF, r_info >> 8
            if kind in RELATIVE:
                continue
            if kind not in ABSOLUTE:
                fail("relocation type %d in %s" % (kind, name(s)))
            sname, value, shndx = symbol(sym)
            if shndx == SHN_ABS:
                continue
            if shndx == SHN_UNDEF:
                if struct.unpack_from("<I", image, r_offset)[0] != 0:
                    fail("undefined symbol " + sname)
                continue   # an unresolved weak reference: stays 0
            if not target[3] <= r_offset < target[3] + target[5] or r_offset % 4:
                fail("relocation outside its section at %#x" % r_offset)
            relocs.add(r_offset)

    header = struct.pack("<6I", MAGIC, ABI, image_end, mem_end - image_end, entry, len(relocs))
    with open(dst, "wb") as f:
        f.write(header + bytes(image) + struct.pack("<%dI" % len(relocs), *sorted(relocs)))
    print("tls.bin: %d bytes of image, %d of zeroed data, %d relocations" % (image_end, mem_end - image_end, len(relocs)))


if __name__ == "__main__":
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    main(sys.argv[1], sys.argv[2])
