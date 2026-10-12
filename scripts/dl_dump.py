# Prints the graphics tasks found in a big-endian RAM dump (Project64 dump, or F9 in adam.exe) and walks
# their display lists, decoding texture loads, tiles, rectangles and S2DEX2 objects.
# Usage: python scripts\dl_dump.py <ramdump.bin> [task index | DL address hex] [--raw]
import struct
import sys

UCODE_NAMES = {}  # ucode text address -> "F3DEX2" / "S2DEX2", filled from the version string in ucode data

FMT = ["RGBA", "YUV", "CI", "IA", "I"]
SIZ = ["4b", "8b", "16b", "32b"]


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    raw = "--raw" in sys.argv
    ram = open(args[0], "rb").read()
    w32 = lambda a: struct.unpack_from(">I", ram, a & 0x3FFFFF)[0]

    tasks = find_tasks(ram)
    for i, t in enumerate(tasks):
        print(f"task {i}: @{t['at']:08X} {t['ucode_name']:7s} DL {t['dl']:08X} size 0x{t['dl_size']:X}")
    if len(args) < 2:
        return
    sel = int(args[1], 16) if len(args[1]) > 3 else None
    for i, t in enumerate(tasks):
        if sel is None and i != int(args[1]):
            continue
        if sel is not None and t["dl"] != sel:
            continue
        print(f"\n=== task {i} ({t['ucode_name']}) DL {t['dl']:08X}")
        walk(ram, w32, t["dl"], t["ucode_name"], raw)
        break


def find_tasks(ram):
    tasks = []
    for o in range(0, len(ram) - 64, 4):
        w = struct.unpack_from(">16I", ram, o)
        if w[0] == 1 and all(0x80000000 <= w[i] < 0x80400000 for i in (2, 4, 6, 12)) and 0 < w[3] <= 0x1000:
            data = ram[(w[6] & 0x3FFFFF):(w[6] & 0x3FFFFF) + 0x800]
            name = "S2DEX2" if b"S2DEX" in data else "F3DEX2" if b"F3DEX" in data else "?"
            tasks.append({"at": 0x80000000 + o, "ucode": w[4], "ucode_name": name, "dl": w[12], "dl_size": w[13]})
    return tasks


def walk(ram, w32, start, ucode, raw):
    seg = [0] * 16
    stack = []
    pc = start
    timg = None
    depth = 0
    n = 0
    while n < 5000:
        n += 1
        a = phys(seg, pc)
        hi, lo = w32(a), w32(a + 4)
        op = hi >> 24
        ind = "  " * len(stack)
        txt = decode(op, hi, lo, ucode, seg, ram, w32)
        if raw or txt:
            print(f"{ind}{pc:08X}: {hi:08X} {lo:08X}  {txt or ''}")
        pc += 8
        if op == 0xDB and ((hi >> 16) & 0xFF) == 6:  # MOVEWORD SEGMENT
            seg[(hi & 0xFFFF) // 4] = lo & 0xFFFFFF
        elif op == 0xDE:  # DL / BRANCH
            if ((hi >> 16) & 0xFF) == 0:
                stack.append(pc)
            pc = lo
        elif op == 0xDF:  # ENDDL
            if not stack:
                break
            pc = stack.pop()
        elif op == 0xE4 or op == 0xE5:  # texrect: 2 more words in F3DEX2 (RDPHALF_1/2)
            pc += 16
        elif op == 0xDD:  # LOAD_UCODE
            ucode = "S2DEX2" if ucode == "F3DEX2" else "F3DEX2"
            print(f"{ind}  (ucode switch -> {ucode}?)")


def phys(seg, addr):
    if addr >= 0x80000000:
        return addr & 0x3FFFFF
    return (seg[(addr >> 24) & 0xF] + (addr & 0xFFFFFF)) & 0x3FFFFF


def decode(op, hi, lo, ucode, seg, ram, w32):
    if op == 0xFD:
        return f"SETTIMG {FMT[(hi >> 21) & 7] if ((hi >> 21) & 7) < 5 else '?'} {SIZ[(hi >> 19) & 3]} width {(hi & 0xFFF) + 1} addr {lo:08X}"
    if op == 0xF5:
        return (f"SETTILE tile {(lo >> 24) & 7} {FMT[(hi >> 21) & 7] if ((hi >> 21) & 7) < 5 else '?'} {SIZ[(hi >> 19) & 3]} line {(hi >> 9) & 0x1FF} "
                f"tmem 0x{(hi & 0x1FF) * 8:X} pal {(lo >> 20) & 15} cmt {(lo >> 18) & 3} maskt {(lo >> 14) & 15} shiftt {(lo >> 10) & 15} "
                f"cms {(lo >> 8) & 3} masks {(lo >> 4) & 15} shifts {lo & 15}")
    if op == 0xF3:
        return f"LOADBLOCK tile {(lo >> 24) & 7} uls {(hi >> 12) & 0xFFF} ult {hi & 0xFFF} texels {((lo >> 12) & 0xFFF) + 1} dxt {lo & 0xFFF}"
    if op == 0xF4:
        return f"LOADTILE tile {(lo >> 24) & 7} ({((hi >> 12) & 0xFFF) / 4},{(hi & 0xFFF) / 4})-({((lo >> 12) & 0xFFF) / 4},{(lo & 0xFFF) / 4})"
    if op == 0xF2:
        return f"SETTILESIZE tile {(lo >> 24) & 7} ({((hi >> 12) & 0xFFF) / 4},{(hi & 0xFFF) / 4})-({((lo >> 12) & 0xFFF) / 4},{(lo & 0xFFF) / 4})"
    if op == 0xF0:
        return f"LOADTLUT tile {(lo >> 24) & 7} count {((lo >> 14) & 0x3FF) + 1}"
    if op == 0xE4:
        return f"TEXRECT ({((lo >> 12) & 0xFFF) / 4},{(lo & 0xFFF) / 4})-({((hi >> 12) & 0xFFF) / 4},{(hi & 0xFFF) / 4}) tile {(lo >> 24) & 7}"
    if op == 0xF6:
        return f"FILLRECT ({((lo >> 12) & 0xFFF) / 4},{(lo & 0xFFF) / 4})-({((hi >> 12) & 0xFFF) / 4},{(hi & 0xFFF) / 4})"
    if op == 0xFF:
        return f"SETCIMG {SIZ[(hi >> 19) & 3]} width {(hi & 0xFFF) + 1} addr {lo:08X}"
    if op == 0xE3:
        return f"SETOTHERMODE_H shift {32 - ((hi >> 8) & 0xFF) - (hi & 0xFF) - 1} len {(hi & 0xFF) + 1} = {lo:08X}"
    if op == 0xE2:
        return f"SETOTHERMODE_L shift {32 - ((hi >> 8) & 0xFF) - (hi & 0xFF) - 1} len {(hi & 0xFF) + 1} = {lo:08X}"
    if op == 0xDE:
        return f"{'DL' if ((hi >> 16) & 0xFF) == 0 else 'BRANCH_DL'} {lo:08X}"
    if op == 0xDF:
        return "ENDDL"
    if op == 0xDD:
        return f"LOAD_UCODE text {lo:08X}"
    if op == 0xDB:
        return f"MOVEWORD idx {(hi >> 16) & 0xFF} ofs {hi & 0xFFFF:X} = {lo:08X}"
    if op == 0xFC:
        return f"SETCOMBINE {hi & 0xFFFFFF:06X} {lo:08X}"
    if ucode == "S2DEX2":
        names = {0x01: "OBJ_RECTANGLE", 0x02: "OBJ_SPRITE", 0x04: "SELECT_DL", 0x05: "OBJ_LOADTXTR", 0x06: "OBJ_LDTX_SPRITE",
                 0x07: "OBJ_LDTX_RECT", 0x08: "OBJ_LDTX_RECT_R", 0x09: "BG_1CYC", 0x0A: "BG_COPY", 0x0B: "OBJ_RENDERMODE",
                 0xDA: "OBJ_RECTANGLE_R", 0xDC: "OBJ_MOVEMEM"}
        if op in names:
            s = f"{names[op]} {lo:08X}"
            p = phys(seg, lo)
            if op in (0x09, 0x0A):  # uObjBg / uObjScaleBg
                b = struct.unpack_from(">HHhhHHhhIHBBHHHH", ram, p)
                s += (f"  image {b[1] / 4}x{b[5] / 4} at ({b[0] / 32},{b[4] / 32}) frame ({b[2] / 4},{b[6] / 4}) {b[3] / 4}x{b[7] / 4}"
                      f" ptr {b[8]:08X} load {'TILE' if b[9] == 0xFC else 'BLOCK' if b[9] == 0x33 else b[9]}"
                      f" {FMT[b[10]] if b[10] < 5 else b[10]}{SIZ[b[11] & 3]} pal {b[12]} flip {b[13]}")
                s += f" scale {b[14] / 1024}x{b[15] / 1024}" if op == 0x09 else f" tmemW {b[14]} tmemH {b[15]}"
            elif op in (0x05, 0x06, 0x07, 0x08):  # uObjTxtr, then uObjSprite for the LDTX variants
                t = struct.unpack_from(">IIHHHHII", ram, p)
                kind = {0x00001033: "BLOCK", 0x00FC1034: "TILE", 0x00000030: "TLUT"}.get(t[0], f"{t[0]:08X}")
                if kind == "TLUT":
                    s += f"  TLUT image {t[1]:08X} tmem 0x{t[2] * 8:X} count {t[3] + 1}"
                elif kind == "BLOCK":
                    s += f"  BLOCK image {t[1]:08X} tmem 0x{t[2] * 8:X} tsize {t[3]} tline {t[4]}"
                else:
                    s += f"  TILE image {t[1]:08X} tmem 0x{t[2] * 8:X} twidth {t[3]} theight {t[4]}"
                if op != 0x05:
                    o = struct.unpack_from(">hHHHhHHHHHBBBB", ram, p + 24)
                    s += (f"\n{' ' * 30}sprite ({o[0] / 4},{o[4] / 4}) scale {o[1] / 1024}x{o[5] / 1024} image {o[2] / 32}x{o[6] / 32}"
                          f" stride {o[8]} adrs 0x{o[9] * 8:X} {FMT[o[10]] if o[10] < 5 else o[10]}{SIZ[o[11] & 3]} pal {o[12]} flags {o[13]}")
            return s
    return None


if __name__ == "__main__":
    main()
