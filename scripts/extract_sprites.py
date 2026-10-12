# Extracts the 2D images (S2DEX2 sprites and backgrounds) of the frame in a big-endian RAM dump as PNG files, and
# looks each image's raw bytes up in the ROM (found = uncompressed there, so the launcher could read it from the
# player's ROM). For finding artwork: look at the PNGs yourself.
# Usage: python scripts\extract_sprites.py <ramdump.bin> <output folder> [--rom rom\game.z64]
import os, struct, sys
import numpy as np
from PIL import Image

BASE = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))  # project folder (scripts\..)
FMT = ["RGBA", "YUV", "CI", "IA", "I"]
SIZ = [4, 8, 16, 32]


def rgba16(v):
    return ((v >> 11) & 31) * 255 // 31, ((v >> 6) & 31) * 255 // 31, ((v >> 1) & 31) * 255 // 31, 255 if v & 1 else 0


def decode(ram, addr, width, height, row_bytes, fmt, siz, palette):
    """Returns an RGBA numpy array or None for unsupported formats."""
    out = np.zeros((height, width, 4), dtype=np.uint8)
    for y in range(height):
        row = addr + y * row_bytes
        for x in range(width):
            if siz == 4:
                b = ram[row + x // 2]
                v = (b >> 4) if x % 2 == 0 else (b & 15)
            elif siz == 8:
                v = ram[row + x]
            elif siz == 16:
                v = struct.unpack_from(">H", ram, row + x * 2)[0]
            else:
                return None
            if fmt == "CI":
                out[y, x] = rgba16(palette[v]) if v < len(palette) else (255, 0, 255, 255)
            elif fmt == "RGBA" and siz == 16:
                out[y, x] = rgba16(v)
            elif fmt == "IA" and siz == 8:
                i = (v >> 4) * 17
                out[y, x] = (i, i, i, (v & 15) * 17)
            elif fmt == "IA" and siz == 4:
                i = (v >> 1) * 255 // 7
                out[y, x] = (i, i, i, 255 if v & 1 else 0)
            elif fmt == "IA" and siz == 16:
                out[y, x] = (v >> 8, v >> 8, v >> 8, v & 255)
            elif fmt == "I":
                i = v * 17 if siz == 4 else v & 255
                out[y, x] = (i, i, i, 255)
            else:
                return None
    return out


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    rom_path = sys.argv[sys.argv.index("--rom") + 1] if "--rom" in sys.argv else os.path.join(BASE, "rom", "game.z64")
    ram = open(args[0], "rb").read()
    rom = open(rom_path, "rb").read()
    out_dir = args[1]
    os.makedirs(out_dir, exist_ok=True)
    w32 = lambda a: struct.unpack_from(">I", ram, a & 0x3FFFFF)[0]
    phys = lambda seg, a: (seg[(a >> 24) & 15] + (a & 0xFFFFFF)) & 0x3FFFFF if (a >> 24) < 16 else a & 0x3FFFFF

    # Graphics tasks (as dl_dump.py finds them); only the S2DEX2 ones draw sprites/backgrounds.
    tasks = []
    for o in range(0, len(ram) - 64, 4):
        w = struct.unpack_from(">16I", ram, o)
        if w[0] == 1 and all(0x80000000 <= w[i] < 0x80400000 for i in (2, 4, 6, 12)) and 0 < w[3] <= 0x1000:
            data = ram[(w[6] & 0x3FFFFF):(w[6] & 0x3FFFFF) + 0x800]
            if b"S2DEX" in data:
                tasks.append(w[12])

    seen = set()
    count = 0
    for dl in dict.fromkeys(tasks):
        seg = [0] * 16
        stack, pc, palette, tlut_tmem = [], dl, [], 0x800
        for _ in range(5000):
            a = phys(seg, pc)
            hi, lo = w32(a), w32(a + 4)
            op = hi >> 24
            pc += 8
            if op == 0xDF:  # ENDDL
                if not stack:
                    break
                pc = stack.pop()
            elif op == 0xDE:  # DL call / branch
                if (hi >> 16) & 0xFF == 0:
                    stack.append(pc)
                pc = lo
            elif op == 0xDB and ((hi >> 16) & 0xFF) == 6:  # MOVEWORD segment
                seg[((hi & 0xFFFF) // 4) & 15] = lo & 0xFFFFFF
            elif op in (0x05, 0x06, 0x07, 0x08):  # OBJ_LOADTXTR / LDTX_SPRITE / LDTX_RECT / LDTX_RECT_R
                p = phys(seg, lo)
                t = struct.unpack_from(">IIHHHHII", ram, p)
                if t[0] == 0x00000030:  # TLUT
                    img = phys(seg, t[1])
                    palette = [struct.unpack_from(">H", ram, img + i * 2)[0] for i in range(t[3] + 1)]
                    tlut_tmem = t[2] * 8
                if op != 0x05:
                    o = struct.unpack_from(">hHHHhHHHHHBBBB", ram, p + 24)
                    width, height = o[2] // 32, o[6] // 32
                    fmt, siz, pal = FMT[o[10]] if o[10] < 5 else "?", SIZ[o[11] & 3], o[12]
                    row_bytes = o[8] * 8
                    img = phys(seg, t[1]) if t[0] in (0x00001033, 0x00FC1034) else None
                    if img is not None:
                        count += save(ram, rom, img, width, height, row_bytes, fmt, siz, palette[pal * 16:] if siz == 4 else palette, out_dir, seen)
            elif op in (0x09, 0x0A):  # BG_1CYC / BG_COPY
                b = struct.unpack_from(">HHhhHHhhIHBBHHHH", ram, phys(seg, lo))
                width, height = b[1] // 4, b[5] // 4
                fmt, siz = FMT[b[10]] if b[10] < 5 else "?", SIZ[b[11] & 3]
                row_bytes = width * siz // 8
                count += save(ram, rom, phys(seg, b[8]), width, height, row_bytes, fmt, siz, palette, out_dir, seen)
    print(f"{count} images written to {out_dir}")


def save(ram, rom, img, width, height, row_bytes, fmt, siz, palette, out_dir, seen):
    key = (img, width, height, fmt, siz)
    if key in seen or width <= 0 or height <= 0 or img + row_bytes * height > len(ram):
        return 0
    seen.add(key)
    pixels = decode(ram, img, width, height, row_bytes, fmt, siz, palette)
    if pixels is None:
        print(f"  skipped {fmt}{siz} {width}x{height} at {0x80000000 + img:08X}")
        return 0
    raw = ram[img:img + row_bytes * height]
    rom_at = rom.find(raw) if len(raw) >= 64 else -1
    where = f"rom{rom_at:07X}" if rom_at >= 0 else "notinrom"
    name = f"{0x80000000 + img:08X}_{width}x{height}_{fmt}{siz}_{where}.png"
    Image.fromarray(pixels, "RGBA").save(os.path.join(out_dir, name))
    print(f"  {name}")
    return 1


if __name__ == "__main__":
    main()
