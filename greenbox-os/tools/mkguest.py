#!/usr/bin/env python3
"""
mkguest.py - turn a linked guest ELF into a .gbx image.

The job is small but fiddly: split the ELF into a text half and a data half,
find every absolute address the two halves contain, rewrite each of those to a
plain offset, and record where they were so the loader can add the real base
back at load time.

Which words are addresses is not guessable - 0x00000010 is equally plausible as
a pointer and as the number sixteen - so the answer comes from the linker.
Linking with --emit-relocs keeps the relocation records in the output, and
every R_XTENSA_32 among them marks exactly one 32-bit word that holds an
address. Everything else (the PC-relative slot relocations) can be ignored,
because text moves as a unit and an l32r keeps reaching its literal.

ELF parsing is done by hand rather than with pyelftools, so that building a
guest needs nothing installed beyond python and the cross-compiler.
"""

import argparse
import os
import re
import struct
import sys
import zlib

# ---------------------------------------------------------------- constants

GB_MAGIC       = 0x31584247          # "GBX1"
GB_NAME_MAX    = 24

HDR_FMT  = "<IHHIIIIII%dsI" % GB_NAME_MAX
HDR_SIZE = struct.calcsize(HDR_FMT)   # 60

REL_OFF_MASK = 0x0FFFFFFF
REL_IN_DATA  = 0x10000000
REL_TO_DATA  = 0x20000000

# Must match guest.ld.
TEXT_ORIGIN = 0x00000000
DATA_ORIGIN = 0x10000000

SHT_RELA   = 4
SHT_NOBITS = 8
R_XTENSA_32 = 1

# Relocation types that are safe to ignore: PC-relative instruction operands
# and assembler bookkeeping. They stay correct when a section moves as a whole.
IGNORED_RELOCS = {
    0,          # R_XTENSA_NONE
    8, 9, 10,   # OP0..OP2
    11, 12,     # ASM_EXPAND, ASM_SIMPLIFY
    14,         # 32_PCREL
    15, 16,     # GNU_VTINHERIT, GNU_VTENTRY
    17, 18, 19, # DIFF8/16/32
}
IGNORED_RELOCS |= set(range(20, 57))   # SLOT0_OP .. SLOT14_ALT


def abi_version():
    """The number in abi/greenbox_abi.h, read rather than repeated here.

    Every image carries the ABI version it was built against and the launcher
    refuses one that does not match the running OS. A second copy of the
    number in this file is a copy that can go stale, and when it does the
    symptom is "wrong ABI - rebuild it" against a guest that was in fact just
    rebuilt - with nothing pointing at the tool.
    """
    path = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                        os.pardir, "abi", "greenbox_abi.h")
    try:
        with open(path) as f:
            text = f.read()
    except OSError as e:
        raise Fail("cannot read %s: %s" % (path, e))

    m = re.search(r"^#define\s+GB_ABI_VERSION\s+(\d+)", text, re.M)
    if not m:
        raise Fail("no GB_ABI_VERSION in %s" % path)
    return int(m.group(1))


class Fail(Exception):
    pass


# -------------------------------------------------------------- ELF reading

class Section:
    __slots__ = ("name", "type", "flags", "addr", "off", "size",
                 "link", "info", "entsize", "data")


def read_elf(path):
    blob = open(path, "rb").read()
    if blob[:4] != b"\x7fELF":
        raise Fail("%s is not an ELF file" % path)
    if blob[4] != 1 or blob[5] != 1:
        raise Fail("expected a 32-bit little-endian ELF")

    (e_shoff,) = struct.unpack_from("<I", blob, 0x20)
    e_shentsize, e_shnum, e_shstrndx = struct.unpack_from("<HHH", blob, 0x2E)
    if e_shoff == 0 or e_shnum == 0:
        raise Fail("no section headers - was the file stripped?")

    raw = []
    for i in range(e_shnum):
        base = e_shoff + i * e_shentsize
        fields = struct.unpack_from("<IIIIIIIIII", blob, base)
        s = Section()
        (name_off, s.type, s.flags, s.addr, s.off, s.size,
         s.link, s.info, _align, s.entsize) = fields
        s.data = (b"" if s.type == SHT_NOBITS
                  else blob[s.off:s.off + s.size])
        s.name = name_off          # resolved below
        raw.append(s)

    strtab = raw[e_shstrndx].data
    for s in raw:
        end = strtab.index(b"\0", s.name)
        s.name = strtab[s.name:end].decode()

    return raw


def find_symbol(sections, want):
    symtab = next((s for s in sections if s.name == ".symtab"), None)
    if symtab is None:
        raise Fail("no .symtab - link without -s")
    strtab = sections[symtab.link].data

    for off in range(0, len(symtab.data), 16):
        st_name, st_value, _size, _info, _other, _shndx = struct.unpack_from(
            "<IIIBBH", symtab.data, off)
        end = strtab.index(b"\0", st_name)
        if strtab[st_name:end].decode() == want:
            return st_value
    raise Fail("symbol %s not found" % want)


# ------------------------------------------------------------- conversion

def build(elf_path, name, stack, verbose):
    sections = read_elf(elf_path)
    by_name = {s.name: s for s in sections}

    text = by_name.get(".text")
    data = by_name.get(".data")
    bss  = by_name.get(".bss")

    if text is None or text.size == 0:
        raise Fail("no .text - check guest.ld and that gb_main is reachable")
    if text.addr != TEXT_ORIGIN:
        raise Fail(".text is at 0x%08x, expected 0x%08x" % (text.addr, TEXT_ORIGIN))
    if data is not None and data.size and data.addr != DATA_ORIGIN:
        raise Fail(".data is at 0x%08x, expected 0x%08x" % (data.addr, DATA_ORIGIN))

    text_img = bytearray(text.data)
    data_img = bytearray(data.data) if data is not None else bytearray()
    bss_len  = bss.size if bss is not None else 0

    while len(text_img) % 4: text_img.append(0)
    while len(data_img) % 4: data_img.append(0)

    text_lo, text_hi = TEXT_ORIGIN, TEXT_ORIGIN + len(text_img)
    data_lo, data_hi = DATA_ORIGIN, DATA_ORIGIN + len(data_img) + bss_len

    entry = find_symbol(sections, "gb_main") - TEXT_ORIGIN
    if not (0 <= entry < len(text_img)):
        raise Fail("gb_main at 0x%08x is outside .text" % (entry + TEXT_ORIGIN))

    # --- collect relocations -------------------------------------------
    relocs = []
    skipped = {}

    for s in sections:
        if s.type != SHT_RELA:
            continue
        target = sections[s.info]
        if target.name == ".text":
            img, img_base, in_data = text_img, TEXT_ORIGIN, False
        elif target.name == ".data":
            img, img_base, in_data = data_img, DATA_ORIGIN, True
        else:
            continue        # relocations against non-allocated sections

        for off in range(0, len(s.data), 12):
            r_offset, r_info, _addend = struct.unpack_from("<IIi", s.data, off)
            rtype = r_info & 0xFF

            if rtype != R_XTENSA_32:
                if rtype not in IGNORED_RELOCS:
                    skipped[rtype] = skipped.get(rtype, 0) + 1
                continue

            site = r_offset - img_base
            if site < 0 or site + 4 > len(img) or site % 4:
                raise Fail("relocation at 0x%08x is outside or misaligned in %s"
                           % (r_offset, target.name))

            (value,) = struct.unpack_from("<I", img, site)

            # The linker has already written the final address here, so the
            # value itself says which half it points into.
            if text_lo <= value <= text_hi:
                normalised, to_data = value - TEXT_ORIGIN, False
            elif data_lo <= value <= data_hi:
                normalised, to_data = value - DATA_ORIGIN, True
            else:
                raise Fail(
                    "relocation at 0x%08x points at 0x%08x, which is in neither "
                    "half. An absolute address that is not part of the image "
                    "cannot be relocated - is the guest referencing an OS "
                    "symbol directly instead of going through the api table?"
                    % (r_offset, value))

            struct.pack_into("<I", img, site, normalised)

            entry_word = site & REL_OFF_MASK
            if in_data: entry_word |= REL_IN_DATA
            if to_data: entry_word |= REL_TO_DATA
            relocs.append(entry_word)

    if skipped and verbose:
        for t, n in sorted(skipped.items()):
            print("  note: ignored %d relocation(s) of type %d" % (n, t))

    # --- pack -----------------------------------------------------------
    rel_blob = struct.pack("<%dI" % len(relocs), *relocs)
    body = bytes(text_img) + bytes(data_img) + rel_blob
    crc = zlib.crc32(body) & 0xFFFFFFFF

    hdr = struct.pack(HDR_FMT,
                      GB_MAGIC, abi_version(), HDR_SIZE,
                      len(text_img), len(data_img), bss_len,
                      entry, len(relocs), stack,
                      name.encode()[:GB_NAME_MAX].ljust(GB_NAME_MAX, b"\0"),
                      crc)

    return hdr + body, dict(text=len(text_img), data=len(data_img),
                            bss=bss_len, relocs=len(relocs), entry=entry)


def info(path):
    blob = open(path, "rb").read()
    if len(blob) < HDR_SIZE:
        raise Fail("%s is too short to be a .gbx" % path)
    (magic, abi, hdr_len, text_len, data_len, bss_len,
     entry, nrel, stack, name, crc) = struct.unpack_from(HDR_FMT, blob, 0)

    if magic != GB_MAGIC:
        raise Fail("bad magic 0x%08x" % magic)

    body = blob[hdr_len:]
    ok = (zlib.crc32(body) & 0xFFFFFFFF) == crc

    print("%s" % path)
    print("  name     %s" % name.rstrip(b"\0").decode())
    want = abi_version()
    print("  abi      %d%s" % (abi, "" if abi == want
                               else "  (the header says %d)" % want))
    print("  text     %6d B   -> executable IRAM" % text_len)
    print("  data     %6d B   -> DRAM" % data_len)
    print("  bss      %6d B" % bss_len)
    print("  relocs   %6d     (%d B)" % (nrel, nrel * 4))
    print("  entry    0x%04x" % entry)
    print("  stack    %d" % (stack or 0))
    print("  total    %6d B on disk, %d B of RAM at runtime"
          % (len(blob), text_len + data_len + bss_len))
    print("  crc      %s" % ("ok" if ok else "MISMATCH"))
    return 0 if ok else 1


def main():
    ap = argparse.ArgumentParser(description=__doc__.strip().splitlines()[0])
    ap.add_argument("input", help="linked guest ELF, or a .gbx with --info")
    ap.add_argument("-o", "--output", help="output .gbx")
    ap.add_argument("-n", "--name", help="display name (default: from -o)")
    ap.add_argument("-s", "--stack", type=int, default=0,
                    help="task stack in bytes, 0 = OS default")
    ap.add_argument("--info", action="store_true", help="dump a .gbx and exit")
    ap.add_argument("-v", "--verbose", action="store_true")
    args = ap.parse_args()

    try:
        if args.info:
            return info(args.input)

        if not args.output:
            raise Fail("-o is required")
        name = args.name or args.output.replace("\\", "/").split("/")[-1]
        if name.endswith(".gbx"):
            name = name[:-4]

        blob, st = build(args.input, name, args.stack, args.verbose)
        open(args.output, "wb").write(blob)

        print("%-10s text %5d  data %5d  bss %5d  relocs %4d  -> %d B"
              % (name, st["text"], st["data"], st["bss"], st["relocs"],
                 len(blob)))
        return 0

    except Fail as e:
        print("mkguest: %s" % e, file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
