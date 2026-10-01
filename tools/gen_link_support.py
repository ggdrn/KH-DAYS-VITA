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
overlays.ld    Linker script: each overlay's .text, .rodata, .data and .bss grouped with
               kh_<ovNNN>_<kind>_start/_end symbols, so the port knows where an overlay lives
               (FS_EndOverlay runs the destructors inside it; a reload restores its .data).
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

sys.path.insert(0, str(Path(__file__).resolve().parent))
import ovdisp  # noqa: E402

# The decomp's tools/configure.py ABSOLUTE_SYMBOLS that are second names of a data symbol.
ALIASES = ("data_ov002_0207e9f4_default", "data_ov002_0207ef80_offsets")

# Names the port's decomp patch folds into a larger object: name -> (object, offset).
#
# The "khdays: shared-bss" groups: a file's zero-initialised statics that mwcc lays out as one
# block after a base symbol. Some functions reach them off the base (data_X.field, data_X[n]),
# others by name; each name without a DS address of its own is pinned to its place in the
# block, or the two kinds of access would see different variables. Offsets from the ROM
# (the functions' loads and stores) or from a struct the matching code already uses.
#
# They are defined in ds_bss.S next to their block (`.set name, block+offset`), never with
# --defsym name=block+offset: ld makes that an absolute symbol, vita-elf-create leaves
# references to absolute symbols unrelocated, and the code then reaches the link-time address
# instead of where the eboot was loaded (0.0.10: OSi_CurrentThreadPtr read as NULL).
SUBOBJECTS = {
    # os_thread.c (OS_InitThread writes data_0204430c.currentThreadPtr; OS_SleepThread reads
    # OSi_CurrentThreadPtr)
    "OSi_RescheduleCount": ("data_0204430c", 0x04),
    "OSi_CurrentThreadPtr": ("data_0204430c", 0x08),
    "OSi_StackForDestructor": ("data_0204430c", 0x1c),
    "OSi_ThreadIdCount": ("data_0204430c", 0x20),
    # snd_command.c (SND_CommandInit's stores; SND_PopFreeCommand etc. index data_02044748[n])
    "sFinishedTag": ("data_02044748", 0x04),
    "sReserveList": ("data_02044748", 0x08),
    "sReserveListEnd": ("data_02044748", 0x0c),
    "sFreeListEnd": ("data_02044748", 0x10),
    "sWaitingCommandListQueueRead": ("data_02044748", 0x14),
    "sWaitingCommandListQueueWrite": ("data_02044748", 0x18),
    "sWaitingCommandListCount": ("data_02044748", 0x1c),
    "sCurrentTag": ("data_02044748", 0x20),
    # NitroSystem resource_mgr.c (NNS_SndAllocAlarm, NNS_SndLockChannel; SndCapture_Reset
    # clears data_0204a2fc[0..2])
    "sAlarmLock": ("data_0204a2fc", 0x04),
    "sChannelLock": ("data_0204a2fc", 0x08),
    # NitroSystem sndarc_stream.c (NNS_SndArcStrmInit, NNSi_SndArcStrm_MakeWaveData)
    "sPrepareThread": ("data_0204ad8c", 0x04),
    "sDecodeBuffer": ("data_0204ad8c", 0x08),
    # ov105 wireless helper: WhStatics at data_ov105_020c04c0 (the WH state functions use it)
    "sWh_nMpFreq": ("data_ov105_020c04c0", 0x04),
    "sWh_nDisconnectReason": ("data_ov105_020c04c0", 0x0c),
    "sWh_pRecvBuffer": ("data_ov105_020c04c0", 0x14),
    "sWh_pSendBuffer": ("data_ov105_020c04c0", 0x1c),
    "sWh_pReceiver": ("data_ov105_020c04c0", 0x20),
    "sWh_nRecvBufferSize": ("data_ov105_020c04c0", 0x28),
    "sWh_nSendBufferSize": ("data_ov105_020c04c0", 0x2c),
    "sWh_nErrCode": ("data_ov105_020c04c0", 0x30),
    "sWh_nConnectMode": ("data_ov105_020c04c0", 0x34),
    "sWh_pJudgeAccept": ("data_ov105_020c04c0", 0x38),
    "sWh_pChildWEPKeyGenerator": ("data_ov105_020c04c0", 0x40),
    "sWh_pWmBuffer": ("data_ov105_020c04c0", 0x4c),
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


def overlay_table(cfg, rom_path, by_addr, archives):
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
        if name in archives:
            sec = ", ".join(f"{{ kh_{name}_{k}_start, kh_{name}_{k}_end }}" for k, _, _ in KINDS)
            for k, _, _ in KINDS:
                decls.add(f"kh_{name}_{k}_start[]"); decls.add(f"kh_{name}_{k}_end[]")
        else:
            sec = ", ".join("{ 0, 0 }" for _ in KINDS)
        rows.append(f"    {{ {oid}, 0x{ram:08x}, 0x{size:x}, 0x{bss:x}, {bss_block}, {entry}, {init},"
                    f" {{ {sec} }} }},")
        if name in have_bss:
            decls.add(f"kh_bss_{name}_start[]"); decls.add(f"kh_bss_{name}_end[]")
    src = ["/* generated by tools/gen_link_support.py -- do not edit */",
           '#include "hw/overlays.h"', ""]
    for d in sorted(decls):
        src.append(f"extern char {d};" if d.endswith("[]") else f"extern void {d}(void);")
    src += ["", f"const KhOverlay kh_overlays[{len(rows)}] = {{"] + rows + ["};",
            f"const int kh_overlay_count = {len(rows)};", ""]
    return "\n".join(src)


KINDS = (
    ("text", ".text", ".text .text.*"),
    ("rodata", ".rodata", ".rodata .rodata.*"),
    ("data", ".data", ".data .data.*"),
    ("bss", ".bss", ".bss .bss.*"),
)


def overlay_archives(decomp):
    """ovNNN -> archive name (the overlay's source directory, see decomp_sources.module_of)."""
    out = {}
    for d in sorted((decomp / "src" / "overlays").glob("*/ov[0-9][0-9][0-9]*")):
        if d.is_dir():
            out[d.name[:5]] = d.name
    return out


DATA_FILE = re.compile(r"_([0-9a-f]{8})\.c$")


DEFINED = re.compile(r"^[A-Za-z_][\w \*]*?\b(data_\w+)\s*(?:\[[^\]]*\])*\s*(?:=|;)", re.M)


def data_files(decomp, archives, by_addr):
    """module -> [(DS address, pattern)]: the data files (data/*_ADDRESS.c) as whole input
    files, and the module's other initialized objects (defined next to code) one by one."""
    out, in_files = {}, set()

    def add_files(mod, arc, folder):
        for f in folder:
            m = DATA_FILE.search(f.name)
            if not m:
                continue
            out.setdefault(mod, []).append((int(m.group(1), 16), f"*{arc}.a:{f.stem}.o"))
            in_files.update(DEFINED.findall(f.read_text(encoding="utf-8", errors="replace")))

    add_files("main", "main", (decomp / "src" / "engine" / "data").glob("*.c"))
    for ov, arc in archives.items():
        add_files(ov, arc, (decomp / "src" / "overlays").glob(f"*/{arc}/data/*.c"))
    for (mod, addr), entries in by_addr.items():
        if mod != "main" and mod not in archives:
            continue
        for n, k in entries:
            if not k.startswith(("function", "bss")) and n not in in_files:
                out.setdefault(mod, []).append((addr, f"*({{kind}}.{n})"))
    for v in out.values():
        v.sort()
    return out


def ds_ordered(files, mod, kind):
    """Input patterns for a module's data files in DS address order.

    Game code reaches from one object into the next even across source files (0.0.32:
    Ov002_OpenCaptionSurfaces fills a config through data_ov002_0207ebf4 and hands over
    data_ov002_0207ec00, 12 bytes on, from the next file). Each data file holds one DS range,
    named after its start; listing the files in that order puts the ranges back to back as on
    the DS, with whatever each file holds kept in its own order. The rest follows through the
    archive wildcard."""
    if kind not in ("rodata", "data"):
        return []
    lines = []
    for addr, pat in files.get(mod, []):
        align = 4 if addr % 4 == 0 else 2 if addr % 2 == 0 else 1
        if pat.startswith("*("):
            lines.append(f"    . = ALIGN({align}); " + pat.replace("{kind}", "." + kind))
        else:
            # KEEP: a table nobody names directly still holds its neighbours' DS offsets. Not the
            # DS's own C++ exception index (it names ARM9_CTOR_START, __strtoul: unused here).
            keep = "exception_index" not in pat
            sel = f"{pat}(.{kind} .{kind}.*)"
            lines.append(f"    . = ALIGN({align}); " + (f"KEEP({sel})" if keep else sel))
    return lines


def overlay_script(archives, files):
    lines = ["/* generated by tools/gen_link_support.py -- do not edit */"]
    for kind, after, inputs in KINDS:
        lines.append("SECTIONS\n{")
        lines.append(f"  .kh_ov_{kind} :\n  {{")
        for ov, arc in sorted(archives.items()):
            lines.append(f"    kh_{ov}_{kind}_start = .;")
            lines += ds_ordered(files, ov, kind)
            lines.append(f"    *{arc}.a:*({inputs})")
            lines.append(f"    kh_{ov}_{kind}_end = .;")
        lines.append("  }")
        lines.append(f"}}\nINSERT AFTER {after};\n")
    # the main module's objects, in their own output sections in front of the rest
    for kind, after, _ in KINDS:
        main = ds_ordered(files, "main", kind)
        if main:
            lines += ["SECTIONS\n{", f"  .kh_main_{kind} :\n  {{"] + main + ["  }",
                      f"}}\nINSERT BEFORE {after};\n"]
    return "\n".join(lines)


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
        if mod == "dtcm":
            # All of DTCM is one block with the DS layout: the SDK reaches its IRQ vector
            # table, check word and stacks as DTCM base + offset (kh_ds_dtcm). Its two
            # initialized objects (the vector table, the IRQ thread queue) are filled at start-up
            # by the decomp patch (os_irq_table_027e0000.c).
            start, end = 0x027e0000, 0x027e4000
            bss = sorted((addr, name) for name, kind, addr in syms if start <= addr < end)
            asm += [f'.section .bss.kh_ds.{mod},"aw",%nobits', ".balign 32",
                    ".global kh_ds_dtcm", "kh_ds_dtcm:",
                    f".global kh_bss_{mod}_start", f"kh_bss_{mod}_start:"]
        elif ".bss" not in ranges:
            continue
        else:
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
        if obj not in names:
            sys.exit(f"gen_link_support: SUBOBJECTS {name}: {obj} is not a generated .bss block symbol")
        asm += [f".global {name}", f".type {name}, %object", f".set {name}, {obj} + 0x{off:x}"]

    archives = overlay_archives(decomp)
    (out / "overlays.ld").write_text(overlay_script(archives, data_files(decomp, archives, by_addr)))
    (out / "overlays.c").write_text(overlay_table(cfg, rom, by_addr, archives))
    tgts = ovdisp.targets(cfg)
    asm += ovdisp.dispatcher_asm(tgts)
    (out / "ds_bss.S").write_text("\n".join(asm) + "\n")
    (out / "bss_names.txt").write_text("\n".join(names) + "\n")
    (out / "link.rsp").write_text("\n".join(link) + "\n")
    print(f"gen_link_support: {len(names)} bss symbols in {total} bytes, {len(link)} aliases,"
          f" {len(SUBOBJECTS)} names inside blocks, {len(tgts)} shared-address overlay calls")
    return 0


if __name__ == "__main__":
    sys.exit(main())
