#!/usr/bin/env python3
"""Mechanical PLATFORM_VITA rewrites of the build tree (build/decomp).

Each pass edits sources in place and is idempotent: a line it already rewrote is left alone.
The DS side of every change is the original text, so the matching build does not change.
After running, ./export_patch.sh records the result in patches/decomp.patch.

    tools/rewrite_decomp.py [--tree build/decomp] [PASS...]

Passes:
  abs-symbols   `extern T NAME[1];` for linker-absolute constants (OVERLAY_n_ID, SDK_*): the
                source takes the symbol's *address* as the number. vita-elf-create cannot
                relocate absolute symbols, so on the Vita NAME becomes a macro that yields the
                same number as an address of the same type.
  hw            Fixed DS addresses of hardware and the shared area become KH_HW(addr)
                (include/nitro/kh_hw.h: the identity on the DS, platform/compat/kh_hw_map.h on
                the Vita). A literal is wrapped only where it is used as an address: right
                after a pointer cast, as the value of a register-address #define, or at a site
                listed in HW_BARE_SITES after review. Masks, flags and comparisons that happen
                to share the value (0x04000000 is also a texture-format bit) are left alone;
                `--report` lists every remaining in-range literal for review.
  vram-sync     The NitroSDK functions that write VRAMCNT get KH_VRAM_SYNC_ON_EXIT() at the top
                of their body, so the port moves the VRAM banks when they return.
  gxcmd         Stores to geometry command registers become KH_GX_CMD(register, value), which the
                Vita build turns into a call to its geometry engine (a plain store is the DS's).
  align64       Struct and union members declared as a plain `long long` get KH_ALIGN64: the
                DS's 4-byte alignment for them on the Vita (GCC would use 8 and pad the struct
                differently). u64/s64 members need nothing: the typedefs carry it.
"""
import argparse
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]

# The DS linker's absolute symbols. OVERLAY_n_ID = n (dsd's lcf); the SDK constants come from
# the decomp's tools/configure.py ABSOLUTE_SYMBOLS (the ROM's literal pools hold them).
SDK_ABSOLUTE = {
    "SDK_SYS_STACKSIZE": 0x00000000,
    "SDK_IRQ_STACKSIZE": 0x00000800,
    "SDK_SECTION_ARENA_DTCM_START": 0x027E0E60,
}

MARK = "/* PLATFORM_VITA: absolute symbol */"


def abs_value(name):
    m = re.fullmatch(r"OVERLAY_(\d+)_ID", name)
    if m:
        return int(m.group(1))
    return SDK_ABSOLUTE.get(name)


DECL = re.compile(
    r"^(?P<indent>[ \t]*)extern\s+(?P<type>[A-Za-z_][\w ]*?)\s*(?P<name>OVERLAY_\d+_ID|SDK_[A-Z_]+)"
    r"\s*(?P<suffix>\[\s*\d*\s*\]|\(\s*void\s*\))?\s*;(?P<rest>.*)$")


def macro_for(indent, typ, name, suffix, value):
    typ = typ.strip()
    if suffix and suffix.startswith("("):  # extern void NAME(void): a function designator
        expr = f"(({typ} (*)(void))0x{value:x})"
    elif suffix:  # extern T NAME[n]: an array lvalue at that address
        n = re.sub(r"\D", "", suffix) or "1"
        expr = f"(*({typ} (*)[{n}])0x{value:x})"
    else:  # extern T NAME: a scalar lvalue at that address
        expr = f"(*({typ} *)0x{value:x})"
    return f"{indent}#define {name} {expr} {MARK}"


def pass_abs_symbols(tree):
    changed = 0
    for path in sorted(list((tree / "src").rglob("*.c")) + list((tree / "libs").rglob("*.c"))):
        text = path.read_text(encoding="utf-8", errors="surrogateescape")
        if "OVERLAY_" not in text and "SDK_" not in text:
            continue
        lines = text.split("\n")
        out = []
        i = 0
        dirty = False
        while i < len(lines):
            line = lines[i]
            m = DECL.match(line)
            already = i >= 2 and lines[i - 1].strip() == "#else" and MARK in lines[i - 2]
            value = abs_value(m.group("name")) if m else None
            if m and value is not None and not already:
                out += ["#ifdef PLATFORM_VITA",
                        macro_for(m.group("indent"), m.group("type"), m.group("name"),
                                  m.group("suffix"), value),
                        "#else", line, "#endif"]
                dirty = True
            else:
                out.append(line)
            i += 1
        if dirty:
            path.write_text("\n".join(out), encoding="utf-8", errors="surrogateescape")
            changed += 1
    return changed


# ---- hw ------------------------------------------------------------------------------------

HW_RANGES = (
    (0x04000000, 0x04002000),  # I/O
    (0x04100000, 0x04100020),  # I/O: IPC FIFO receive, card data
    (0x05000000, 0x05000800),  # palettes
    (0x06000000, 0x068a4000),  # VRAM views and LCDC
    (0x07000000, 0x07000800),  # OAM
    (0x027ff000, 0x02800000),  # main RAM shared area
    (0x027e0000, 0x027e4000),  # DTCM
    (0x08000000, 0x0a010000),  # GBA slot: ROM and SRAM (read as an empty slot)
    (0xffff0020, 0xffff00bc),  # ARM9 BIOS: only the Nintendo logo, which CTRDG copies
)
HW_INCLUDE = '#include "nitro/kh_hw.h"'
HW_MARK = "KH_HW("

# Literals used as addresses outside a cast, after review (file, literal). See --report.
# Everything else the report lists is a mask, flag or command id (CARD_COMMAND_MASK,
# DISPCNT & 0x07000000, a 64-bit flag) or an argument the callee ignores (Ov003_SceneInit's
# fourth CamAnim_Start argument).
HW_BARE_SITES = {
    # the BG base of engine A or B picked by a ternary, then cast
    ("libs/nitro/nns/calls/BgCharVram_Upload.c", 0x06000000),
    ("libs/nitro/nns/calls/BgCharVram_Upload.c", 0x06200000),
    ("libs/nns/g2d/calls/LoadBGCharacter.c", 0x06000000),
    ("libs/nns/g2d/calls/LoadBGCharacter.c", 0x06200000),
    # the Nintendo logo in the ARM9 BIOS, copied by CTRDGi_InitModuleInfo
    ("include/nitro/ctrdg.h", 0xffff0020),
}

# A register #define whose name says it is a field of the register, not its address: its value
# can fall in an address range all the same (REG_G3X_GXSTAT_GE_MASK is 0x08000000, the GBA
# slot's base; wrapped, the geometry engine always read as busy).
NOT_AN_ADDRESS = re.compile(r"_(MASK|SHIFT|SIZE|BIT|BITS|FLAG|FLAGS|NUM|COUNT)$")

LIT = re.compile(r"(?<![\w.])0[xX]0*([0-9a-fA-F]{7,8})[uUlL]*(?![\w.])")


def in_hw_range(v):
    return any(a <= v < b for a, b in HW_RANGES)


def code_mask(text):
    """True for each character that is code (not in a comment, string or char literal)."""
    mask = bytearray(b"\x01") * len(text)
    i, n = 0, len(text)
    while i < n:
        c = text[i]
        if text.startswith("/*", i):
            j = text.find("*/", i + 2)
            j = n if j < 0 else j + 2
        elif text.startswith("//", i):
            j = text.find("\n", i)
            j = n if j < 0 else j
        elif c in "\"'":
            j = i + 1
            while j < n and text[j] != c:
                j += 2 if text[j] == "\\" else 1
            j += 1
        else:
            i += 1
            continue
        for k in range(i, min(j, n)):
            mask[k] = 0
        i = j
    return mask


def classify(text, start, end):
    """'cast', 'define' or 'bare' for the literal at text[start:end]."""
    before = text[max(0, start - 200):start]
    if re.search(r"\*\s*\)\s*\(*\s*$", before):
        return "cast"
    # `((void (*)(void))0x027e0e60)`: a cast to a function pointer (abs-symbols' SDK constants)
    if re.search(r"\(\s*\*\s*\)\s*\([^()]*\)\s*\)\s*$", before):
        return "cast"
    # `(T *)(offset + 0x06000000)`: the last term of a sum that is cast to a pointer
    if re.search(r"\*\s*\)\s*\(\s*[\w\s+*()]*?\+\s*$", before):
        return "cast"
    # first argument of a helper that takes a register address (G2x_SetBlendAlpha_, BG_CONTROL)
    if re.search(r"\b(G2x_|GXx_|G2S?_|GXS?_|BG_CONTROL)\w*\s*\(\s*$", before):
        return "cast"
    line_start = text.rfind("\n", 0, start) + 1
    line = text[line_start:start]
    m = re.match(r"\s*#\s*define\s+(\w*(?:REG|reg|ADDR|_BASE|HW_|VRAM|PLTT|OAM|LCDC)\w*)\s+\(?\s*$", line)
    if m and not NOT_AN_ADDRESS.search(m.group(1)):
        return "define"
    return "bare"


# Files whose in-range literals are not addresses to translate at compile time.
HW_SKIP_FILES = {
    # LCDC block tables stored as address >> 12 in u16; the readers translate (HW_EDITS)
    "libs/nitro/gx/auto/gx_load3d_tables.c",
}

# Addresses assembled at run time: (file, original text, replacement). KH_HW is the identity
# on the DS, so the replacement compiles to the same code there.
HW_EDITS = (
    ("libs/nitro/gx/calls/GX_BeginLoadTex.c",
     "sTexLCDCBlk1 = (u32)(sTexStartAddrTable[sTex].blk1 << 12);",
     "sTexLCDCBlk1 = (u32)KH_HW(sTexStartAddrTable[sTex].blk1 << 12);"),
    ("libs/nitro/gx/calls/GX_BeginLoadTex.c",
     "sTexLCDCBlk2 = (u32)(sTexStartAddrTable[sTex].blk2 << 12);",
     "sTexLCDCBlk2 = (u32)KH_HW(sTexStartAddrTable[sTex].blk2 << 12);"),
    ("libs/nitro/gx/calls/GX_BeginLoadTexPltt.c",
     "data_0204470c[2] = data_02041418[mask >> 4] << 12;",
     "data_0204470c[2] = KH_HW(data_02041418[mask >> 4] << 12);"),
)


def apply_hw_edits(tree):
    changed = 0
    for rel, old, new in HW_EDITS:
        path = tree / rel
        text = path.read_text(encoding="utf-8", errors="surrogateescape")
        if new in text:
            continue
        if old not in text:
            sys.exit(f"hw: {rel}: expected text not found: {old}")
        text = text.replace(old, new)
        if HW_INCLUDE not in text:
            text = HW_INCLUDE + "  /* PLATFORM_VITA: fixed DS addresses */\n" + text
        path.write_text(text, encoding="utf-8", errors="surrogateescape")
        changed += 1
    return changed


# `HW_MAIN_MEM + 0x007ffxxx`, spelled out or not: a main-RAM address in the shared area.
MAINMEM_SUM = re.compile(r"(?<!KH_HW)\(\s*(0x0*2000000|HW_MAIN_MEM)\s*\+\s*0x0*(7ff[0-9a-fA-F]{3})\s*\)")


def wrap_mainmem_sums(text):
    mask = code_mask(text)
    out, pos = [], 0
    for m in MAINMEM_SUM.finditer(text):
        if not mask[m.start()]:
            continue
        out += [text[pos:m.start()], f"KH_HW{m.group(0)}"]
        pos = m.end()
    if not out:
        return text, False
    out.append(text[pos:])
    return "".join(out), True


def rewrite_hw_text(path, text, report):
    text, summed = wrap_mainmem_sums(text)
    mask = code_mask(text)
    out, pos, changed = [], 0, summed
    for m in LIT.finditer(text):
        if not mask[m.start()]:
            continue
        value = int(m.group(1), 16)
        if not in_hw_range(value):
            continue
        if text[max(0, m.start() - len(HW_MARK)):m.start()] == HW_MARK:
            continue  # already wrapped
        kind = classify(text, m.start(), m.end())
        if kind == "bare" and (path, value) not in HW_BARE_SITES:
            line_no = text.count("\n", 0, m.start()) + 1
            report.append(f"{path}:{line_no}: {text[text.rfind(chr(10), 0, m.start()) + 1:text.find(chr(10), m.end())].strip()}")
            continue
        out.append(text[pos:m.start()])
        out.append(f"KH_HW({m.group(0)})")
        pos = m.end()
        changed = True
    if not changed:
        return None
    out.append(text[pos:])
    new = "".join(out)
    if HW_INCLUDE not in new:
        new = HW_INCLUDE + "  /* PLATFORM_VITA: fixed DS addresses */\n" + new
    return new


def pass_hw(tree, report_path=None):
    changed = 0
    report = []
    files = sorted(list((tree / "src").rglob("*.c")) + list((tree / "src").rglob("*.cpp")) +
                   list((tree / "libs").rglob("*.c")) + list((tree / "include").rglob("*.h")))
    for path in files:
        rel = path.relative_to(tree).as_posix()
        if ("asm_stubs" in rel or "ov028_dsprotect" in rel or rel == "include/nitro/kh_hw.h"
                or rel in HW_SKIP_FILES):
            continue
        text = path.read_text(encoding="utf-8", errors="surrogateescape")
        new = rewrite_hw_text(rel, text, report)
        if new is not None:
            path.write_text(new, encoding="utf-8", errors="surrogateescape")
            changed += 1
    changed += apply_hw_edits(tree)
    if report_path:
        Path(report_path).write_text("\n".join(report) + "\n")
    print(f"hw: {len(report)} in-range literals left as they are (not used as an address)")
    return changed


# ---- vram-sync ---------------------------------------------------------------------------

VRAMCNT_WRITERS = (
    "libs/nitro/gx/auto/GX_VRAMCNT_SetLCDC_.c",
    "libs/nitro/gx/calls/GX_InitGXState.c",
    "libs/nitro/gx/calls/GX_SetBankForBG.c",
    "libs/nitro/gx/calls/GX_SetBankForBGExtPltt.c",
    "libs/nitro/gx/calls/GX_SetBankForOBJ.c",
    "libs/nitro/gx/calls/GX_SetBankForOBJExtPltt.c",
    "libs/nitro/gx/calls/GX_SetBankForSubBG.c",
    "libs/nitro/gx/calls/GX_SetBankForSubBGExtPltt.c",
    "libs/nitro/gx/calls/GX_SetBankForSubOBJ.c",
    "libs/nitro/gx/calls/GX_SetBankForSubOBJExtPltt.c",
    "libs/nitro/gx/calls/GX_SetBankForTex.c",
    "libs/nitro/gx/calls/GX_SetBankForTexPltt.c",
    "libs/nitro/gx/calls/disableBankForX_.c",
    "libs/nitro/gx/calls/resetBankForX_.c",
)
SYNC_BLOCK = "#ifdef PLATFORM_VITA\n    KH_VRAM_SYNC_ON_EXIT();\n#endif\n"


def function_bodies(text, mask):
    """Offsets just after the `{` of each top-level function body."""
    depth, out = 0, []
    for i, c in enumerate(text):
        if not mask[i]:
            continue
        if c == "{":
            if depth == 0 and re.search(r"\)\s*$", text[max(0, i - 400):i]):
                out.append(i + 1)
            depth += 1
        elif c == "}":
            depth -= 1
    return out


def pass_vram_sync(tree):
    changed = 0
    for rel in VRAMCNT_WRITERS:
        path = tree / rel
        text = path.read_text(encoding="utf-8", errors="surrogateescape")
        if "KH_VRAM_SYNC_ON_EXIT" in text:
            continue
        for at in reversed(function_bodies(text, code_mask(text))):
            text = text[:at] + "\n" + SYNC_BLOCK + text[at:].lstrip("\n")
        if HW_INCLUDE not in text:
            text = HW_INCLUDE + "  /* PLATFORM_VITA: fixed DS addresses */\n" + text
        path.write_text(text, encoding="utf-8", errors="surrogateescape")
        changed += 1
    return changed


# ---- align64 -------------------------------------------------------------------------------

RAW64 = re.compile(r"^\s*(?:volatile\s+|const\s+)*(?:(?:unsigned|signed)\s+)?"
                   r"long\s+long(?:\s+int)?\s+\**\s*\w+[^;(]*;")
ALIGN64 = "KH_ALIGN64"


def raw64_member_lines(text):
    """1-based numbers of the lines that declare a plain long long member of a struct/union."""
    t = re.sub(r"/\*.*?\*/", lambda m: "\n" * m.group(0).count("\n"), text, flags=re.S)
    t = re.sub(r"//[^\n]*", "", t)
    stack, pending, out = [], "", []
    for no, line in enumerate(t.split("\n"), 1):
        for i, ch in enumerate(line):
            if ch == "{":
                head = (pending + line[:i])[-200:]
                stack.append(bool(re.search(r"\b(struct|union)\b[^;{}()]*$", head)))
                pending = ""
            elif ch == "}":
                if stack:
                    stack.pop()
                pending = ""
            elif ch == ";":
                pending = ""
        pending += line + "\n"
        if stack and stack[-1] and RAW64.match(line):
            out.append(no)
    return out


RAW64_DECL = re.compile(r"\b(?:(?:unsigned|signed)\s+)?long\s+long\s+(?:int\s+)?([^;(){}]*);")


def raw64_member_sites(text):
    """Offsets of the ';' (and ',') ending plain long long member declarators of a struct or
    union, wherever they sit: one per line or a whole struct on one line (0.0.37: ov005's
    `typedef struct Tween {...; long long startTick; ...}` moved the result screen's menuText
    by 0x10 and the screen freed garbage)."""
    # comments blanked to spaces, so offsets stay those of the original text
    t = re.sub(r"/\*.*?\*/", lambda m: re.sub(r"[^\n]", " ", m.group(0)), text, flags=re.S)
    t = re.sub(r"//[^\n]*", lambda m: " " * len(m.group(0)), t)
    in_struct = [False] * (len(t) + 1)
    stack, last = [], 0
    for i, ch in enumerate(t):
        if ch == "{":
            head = t[max(last, i - 200):i]
            stack.append(bool(re.search(r"\b(struct|union)\b[^;{}()]*$", head)))
            last = i + 1
        elif ch == "}":
            if stack:
                stack.pop()
            last = i + 1
        elif ch == ";":
            last = i + 1
        in_struct[i] = bool(stack and stack[-1])
    sites = []
    for m in RAW64_DECL.finditer(t):
        if not in_struct[m.start()] or ALIGN64 in m.group(1) or "aligned" in m.group(1):
            continue
        # the statement must begin with the type (a member, not `x = (long long)y;`)
        before = re.sub(r"(?:\b(?:volatile|const)\s*)+$", "", t[max(0, m.start() - 80):m.start()].rstrip())
        before = before.rstrip()
        if before and before[-1] not in ";{}":
            continue
        decl_start = m.start(1)
        for j, ch in enumerate(m.group(1)):
            if ch == ",":
                sites.append(decl_start + j)
        sites.append(m.end() - 1)
    return sites


def pass_align64(tree):
    changed = 0
    for path in sorted(tree.rglob("*")):
        if path.suffix not in (".c", ".h", ".cpp") or not path.is_file():
            continue
        rel = path.relative_to(tree).as_posix()
        if not rel.startswith(("include/", "libs/", "src/")):
            continue
        text = path.read_text(encoding="utf-8", errors="surrogateescape")
        if "long long" not in text:
            continue
        sites = raw64_member_sites(text)
        if not sites:
            continue
        for at in sorted(sites, reverse=True):
            text = text[:at].rstrip() + " " + ALIGN64 + text[at:]
        if HW_INCLUDE not in text:
            text = HW_INCLUDE + "  /* PLATFORM_VITA: KH_ALIGN64 */\n" + text
        path.write_text(text, encoding="utf-8", errors="surrogateescape")
        changed += 1
    return changed


# ---- gxcmd ---------------------------------------------------------------------------------
# Geometry commands written straight to their registers (reg_G3_X = v, the GX FIFO port, or a
# raw 0x04000400-0x040005ff store) become KH_GX_CMD(register, v): the store itself on the DS, a
# call into the port's geometry engine on the Vita, where a plain store would only land in
# memory. reg_G3X_* other than the FIFO are control registers the renderer reads: left alone.

GX_LVALUE = re.compile(
    r"(?P<lv>\breg_G3_\w+|\breg_G3X_GXFIFO\b|\*\s*\(\s*[\w ]+\*\s*\)\s*KH_HW\(\s*0x0*4000[45][0-9a-fA-F]{2}\s*\)"
    r"|\*\s*\(\s*REGType32v\s*\*\s*\)\s*\(\s*REG_\w+_ADDR\b[^;=]*\)"
    # the I/O base plus an offset (NNS_G3dGetCurrentMtx's own G3_MtxMode, G3_PushMtx, ...)
    r"|\(\s*\*\s*\(\s*[\w ]+\*\s*\)\s*\(\s*KH_HW\(\s*0x0*4000000\s*\)\s*\+\s*0x[45][0-9a-fA-F]{2}\s*\)\s*\))"
    r"\s*=(?!=)\s*(?P<v>[^;{}]+);")
GX_MARK = "KH_GX_CMD("
_GX_ADDR = r"KH_HW\(\s*0x0*4000[45][0-9a-fA-F]{2}\s*\)"
GX_ALIAS_MACRO = re.compile(r"^\s*#\s*define\s+(?!reg_G3)(\w+)\s+\(\s*\*\s*\(\s*volatile[\w ]*\*\s*\)\s*"
                            + _GX_ADDR + r"\s*\)", re.M)
GX_ALIAS_PTR = re.compile(r"\bvolatile\s+[\w ]+\*\s*(\w+)\s*=\s*\(\s*volatile[\w ]*\*\s*\)\s*"
                          + _GX_ADDR + r"\s*;")


def text_line_is_comment(text, pos):
    bol = text.rfind("\n", 0, pos) + 1
    return text[bol:pos].lstrip().startswith(("/*", "*", "//", "#"))


def pass_gxcmd(tree):
    changed = 0
    for path in sorted(tree.rglob("*")):
        if path.suffix not in (".c", ".h", ".cpp") or not path.is_file():
            continue
        rel = path.relative_to(tree).as_posix()
        if not rel.startswith(("include/", "libs/", "src/")):
            continue
        text = path.read_text(encoding="utf-8", errors="surrogateescape")
        if "reg_G3" not in text and "KH_HW(0x0400" not in text and "KH_HW(0x400" not in text:
            continue
        original = text
        # Names that stand for a geometry port in this file: macros defined as one, and pointer
        # variables initialised with one (main.c's REG_0540, Ov001_SetupDisplayRegs'
        # *reg_swap_buffers, NODEMIX's registers->mode).
        extra = []
        for m in GX_ALIAS_MACRO.finditer(text):
            extra.append(r"\b" + m.group(1) + r"\b")
        for m in GX_ALIAS_PTR.finditer(text):
            n = m.group(1)
            extra += [r"\*\s*" + n + r"\b(?!\s*\[)", r"\b" + n + r"\s*->\s*\w+", r"\b" + n + r"\s*\[[^\]]+\]"]
        if extra:
            # statements only (the start of a line), never the pointer's own declaration
            alias = re.compile(r"^(?P<ind>[ \t]*)(?P<lv>" + "|".join(extra) +
                               r")\s*=(?!=)\s*(?P<v>[^;{}]+);", re.M)
            text = alias.sub(lambda m: f"{m.group('ind')}KH_GX_CMD({m.group('lv')}, {m.group('v').strip()});",
                             text)
        # Whole text, not line by line: a value may span lines (G3_PolygonAttr).
        def sub(m):
            bol = text.rfind("\n", 0, m.start()) + 1
            if text[bol:m.start()].lstrip().startswith(("/*", "*", "//")):
                return m.group(0)
            return f"KH_GX_CMD({m.group('lv')}, {m.group('v').strip()});"
        new = GX_LVALUE.sub(sub, text)
        dirty = new != original
        if dirty:
            text = new
            if HW_INCLUDE not in text:
                text = HW_INCLUDE + "  /* PLATFORM_VITA: KH_GX_CMD */\n" + text
            path.write_text(text, encoding="utf-8", errors="surrogateescape")
            changed += 1
    return changed


PASSES = {"abs-symbols": pass_abs_symbols, "hw": pass_hw, "vram-sync": pass_vram_sync,
          "align64": pass_align64, "gxcmd": pass_gxcmd}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--tree", default=str(ROOT / "build" / "decomp"))
    ap.add_argument("--report", help="hw: write the in-range literals left alone to this file")
    ap.add_argument("passes", nargs="*", default=list(PASSES))
    args = ap.parse_args()
    tree = Path(args.tree)
    for name in args.passes:
        n = PASSES[name](tree, args.report) if name == "hw" else PASSES[name](tree)
        print(f"{name}: {n} files changed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
