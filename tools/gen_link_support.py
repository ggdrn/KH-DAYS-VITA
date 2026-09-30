#!/usr/bin/env python3
"""Generate what the DS linker used to provide, from the decomp's dsd config.

    gen_link_support.py DECOMP OUTDIR ROM

Writes into OUTDIR:

ds_bss.S       Every module's .bss as one block with the DS layout: each bss symbol of
               symbols.txt is a label at its original offset. The decomp reconstructs
               initialized data in C, but most .bss exists only as addresses. Keeping one block
               per module also keeps neighbours adjacent, and game code relies on that: fields
               of a struct can have their own symbol, and loops can run past a table.
               kh_bss_<module>_start/_end bound each block, so loading an overlay can clear it.
bss_names.txt  Those labels. tools/weaken_unowned.py weakens any C definition of them, so the
               block is the single copy.
link.rsp       Linker options: aliases for symbols the DS linker placed at the address of
               another (a second name for the same table).
overlays.c     kh_overlays[]: per overlay its DS load address and sizes, its .bss block, its
               entry and its static initializer. The entry is the function at the load address:
               the game calls an overlay by jumping to FSOverlayInfo's ram_address
               (Ov107_LoadEnemyOverlay: an enemy overlay starts with its RegisterEntityClass),
               so nothing references it by name. The static initializer (NitroSDK "sinit") is
               null in every overlay of this ROM, but is kept for FS_StartOverlay's sake.
               The addresses come from the ROM's overlay table (ROM) and are resolved to names
               with symbols.txt: only names end up in the build, no ROM bytes.
"""
import struct
import re
import sys
from pathlib import Path

# The decomp's tools/configure.py ABSOLUTE_SYMBOLS that are second names of a data symbol.
ALIASES = ("data_ov002_0207e9f4_default", "data_ov002_0207ef80_offsets")

# Names the port's decomp patch folds into a larger object: name -> (object, offset).
SUBOBJECTS = {
    # ov107_tables_020cb630.c: the 16-byte object at 0x020cb628 (see the comment there)
    "data_ov107_020cb630": ("data_ov107_020cb628", 8),
}


def modules(cfg):
    yield "main", cfg
    for name in ("itcm", "dtcm"):
        yield name, cfg / name
    for d in sorted((cfg / "overlays").iterdir()):
        yield d.name, d


def section_ranges(delinks):
    ranges = {}
    for line in delinks.read_text(encoding="utf-8").splitlines():
        if not line.strip():
            break
        m = re.match(r"\s+(\.\w+)\s+start:(0x[0-9a-f]+)\s+end:(0x[0-9a-f]+)", line)
        if m:
            ranges[m.group(1)] = (int(m.group(2), 16), int(m.group(3), 16))
    return ranges


def symbols(symfile):
    for line in symfile.read_text(encoding="utf-8").splitlines():
        m = re.match(r"(\S+) kind:(\S+) addr:(0x[0-9a-f]+)", line)
        if m:
            yield m.group(1), m.group(2), int(m.group(3), 16)


def blz_decompress(data):
    """The DS's backward LZ (MIi_UncompressBackward): the tail of the image is compressed and is
    expanded from the end towards the start; a footer gives the sizes."""
    buf = bytearray(data)
    header_len = buf[-5]
    enc_len = int.from_bytes(buf[-8:-5], "little")
    extra = int.from_bytes(buf[-4:], "little")
    out = bytearray(len(buf) + extra)
    out[:len(buf)] = buf
    src, dst, stop = len(buf) - header_len, len(out), len(buf) - enc_len
    while src > stop:
        src -= 1
        flags = buf[src]
        for _ in range(8):
            if src <= stop:
                break
            if flags & 0x80:
                b1, b2 = buf[src - 1], buf[src - 2]
                src -= 2
                length = (b1 >> 4) + 3
                disp = (((b1 & 0xf) << 8) | b2) + 3
                for _ in range(length):
                    dst -= 1
                    out[dst] = out[dst + disp]
            else:
                src -= 1
                dst -= 1
                out[dst] = buf[src]
            flags = (flags << 1) & 0xff
    return bytes(out)


def overlay_table(cfg, rom_path, by_addr):
    rom = rom_path.read_bytes()
    ovt_off, ovt_size = struct.unpack_from("<II", rom, 0x50)
    fat_off = struct.unpack_from("<I", rom, 0x48)[0]
    have_bss = {d.name for _, d in modules(cfg) if d.parent.name == "overlays"
                and ".bss" in section_ranges(d / "delinks.txt")}
    decls, rows = set(), []
    for i in range(ovt_size // 32):
        oid, ram, size, bss, s0, s1, fid, flags = struct.unpack_from("<8I", rom, ovt_off + i * 32)
        start, end = struct.unpack_from("<II", rom, fat_off + fid * 8)
        image = rom[start:end]
        if (flags >> 24) & 1:  # FSOverlayInfo: compressed size in bits 0-23, flag in bit 24
            image = blz_decompress(image[:flags & 0xffffff])
        name = f"ov{oid:03d}"
        inits = []
        for a in range(s0, s1, 4):
            ptr = struct.unpack_from("<I", image, a - ram)[0]
            if ptr == 0:  # FS_StartOverlay skips null entries
                continue
            fn = next((n for n, k in by_addr.get((name, ptr), []) if k.startswith("function")), None)
            if fn is None:
                sys.exit(f"gen_link_support: {name} static initializer {ptr:08x} has no name")
            inits.append(fn)
            decls.add(fn)
        init = inits[0] if inits else "0"
        if len(inits) > 1:
            sys.exit(f"gen_link_support: {name} has {len(inits)} static initializers")
        entry = next((n for n, k in by_addr.get((name, ram), []) if k.startswith("function")), None)
        # ov028 (DS Protect) is not built: the port answers its checks (platform/nitro/dsprotect.c)
        if entry is None or name == "ov028":
            entry = "0"
        else:
            decls.add(entry)
        bss_block = (f"kh_bss_{name}_start, kh_bss_{name}_end" if name in have_bss else "0, 0")
        rows.append(f"    {{ {oid}, 0x{ram:08x}, 0x{size:x}, 0x{bss:x}, {bss_block}, {entry}, {init} }},")
        if name in have_bss:
            decls.add(f"kh_bss_{name}_start[]"); decls.add(f"kh_bss_{name}_end[]")
    src = ["/* generated by tools/gen_link_support.py -- do not edit */",
           '#include "hw/overlays.h"', ""]
    for d in sorted(decls):
        src.append(f"extern char {d};" if d.endswith("[]") else f"extern void {d}(void);")
    src += ["", f"const KhOverlay kh_overlays[{len(rows)}] = {{"] + rows + ["};",
            f"const int kh_overlay_count = {len(rows)};", ""]
    return "\n".join(src)


def main():
    decomp, out, rom = Path(sys.argv[1]), Path(sys.argv[2]), Path(sys.argv[3])
    out.mkdir(parents=True, exist_ok=True)
    cfg = decomp / "config" / "arm9"
    asm = ["@ generated by tools/gen_link_support.py -- do not edit", ".syntax unified", ""]
    names = []
    by_addr = {}
    total = 0
    for mod, d in modules(cfg):
        ranges = section_ranges(d / "delinks.txt")
        syms = list(symbols(d / "symbols.txt"))
        for name, kind, addr in syms:
            by_addr.setdefault((mod, addr), []).append((name, kind))
        if ".bss" not in ranges:
            continue
        start, end = ranges[".bss"]
        bss = sorted((addr, name) for name, kind, addr in syms
                     if kind == "bss" and start <= addr < end)
        asm += [f'.section .bss.kh_ds.{mod},"aw",%nobits', ".balign 32",
                f".global kh_bss_{mod}_start", f"kh_bss_{mod}_start:"]
        pos = start
        for addr, name in bss:
            if addr > pos:
                asm.append(f"    .space {addr - pos}")
                pos = addr
            asm += [f".global {name}", f".type {name}, %object", f"{name}:"]
            names.append(name)
        if end > pos:
            asm.append(f"    .space {end - pos}")
        asm += [f".global kh_bss_{mod}_end", f"kh_bss_{mod}_end:", ""]
        total += end - start

    link = []
    for alias in ALIASES:
        target = None
        for (mod, addr), entries in by_addr.items():
            if any(n == alias for n, _ in entries):
                target = next((n for n, _ in entries if n != alias), None)
                break
        if target:
            link.append(f"-Wl,--defsym={alias}={target}")
    for name, (obj, off) in SUBOBJECTS.items():
        link.append(f"-Wl,--defsym={name}={obj}+{off}")

    (out / "overlays.c").write_text(overlay_table(cfg, rom, by_addr))
    (out / "ds_bss.S").write_text("\n".join(asm) + "\n")
    (out / "bss_names.txt").write_text("\n".join(names) + "\n")
    (out / "link.rsp").write_text("\n".join(link) + "\n")
    print(f"gen_link_support: {len(names)} bss symbols in {total} bytes, {len(link)} aliases")
    return 0


if __name__ == "__main__":
    sys.exit(main())
