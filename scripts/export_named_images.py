# Exports the ROM's named 8-bit images as PNGs. They are Yay0 files that unpack to a 16-byte header - width, height
# (16-bit each), 4 more bytes, an 8-letter ASCII name (e.g. "D07SYA01") - followed by width x height CI8 pixels.
# Colours: the game loads these images through an asset table (func_8009930C: 12-byte entries {offset, packed size,
# unpacked size}; images base ROM 0x1D9000, table 0x800D6AD0) and their palette through a second one (base ROM
# 0x60CB80, table 0x800DD820; 256 raw RGBA16 colours); the pair list at 0x800E2080 (u16 image, u16 palette,
# indexed by image number) says which palette goes with which image (func_800A9898). Images not in that table
# come out greyscale (colour index as brightness).
# Usage: python scripts\export_named_images.py [output folder]   (default dumps\inbox\artwork_colour)
import os, struct, sys
import numpy as np
from PIL import Image

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import yay0

BASE = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))  # project folder (scripts\..)

IMAGE_ROM, IMAGE_TABLE = 0x1D9000, 0x800D6AD0
PALETTE_ROM, PALETTE_TABLE = 0x60CB80, 0x800DD820
PAIRS = 0x800E2080


def boot_rom(vram):
    """ROM offset of a boot-section address (.text at ROM 0x1000 = VRAM 0x80096400)."""
    return vram - 0x80096400 + 0x1000


def table_entries(rom, base, table):
    """(index, rom address, packed size, unpacked size) of each asset table entry until one looks invalid."""
    index = 0
    while True:
        offset, packed, size = struct.unpack_from(">III", rom, boot_rom(table) + index * 12)
        if size == 0 or size > 0x100000 or base + offset >= len(rom):
            return
        yield index, base + offset, packed, size
        index += 1


def load_palette(rom, index):
    offset, packed, size = struct.unpack_from(">III", rom, boot_rom(PALETTE_TABLE) + index * 12)
    at = PALETTE_ROM + offset
    data = yay0.decompress(rom, at) if packed else rom[at:at + size]
    colours = np.frombuffer(data[:512], dtype=">u2").astype(np.uint32)
    if len(colours) < 256:
        return None
    rgb = np.stack([(colours >> 11) & 31, (colours >> 6) & 31, (colours >> 1) & 31], -1)
    return (rgb * 255 // 31).astype(np.uint8)


def main():
    out = sys.argv[1] if len(sys.argv) > 1 else os.path.join(BASE, "dumps", "inbox", "artwork_colour")
    os.makedirs(out, exist_ok=True)
    rom = open(os.path.join(BASE, "rom", "game.z64"), "rb").read()
    palette_of = {}  # image ROM address -> palette index
    for index, at, _, _ in table_entries(rom, IMAGE_ROM, IMAGE_TABLE):
        image_index, palette_index = struct.unpack_from(">HH", rom, boot_rom(PAIRS) + index * 4)
        if image_index == index:
            palette_of[at] = palette_index
    count = coloured = 0
    for at in yay0.find_all(rom):
        if struct.unpack_from(">I", rom, at + 4)[0] < 0x1010:
            continue
        data = yay0.decompress(rom, at)
        width, height = struct.unpack_from(">HH", data, 0)
        name = data[8:16]
        if not (64 <= width <= 640 and 32 <= height <= 480 and width * height + 16 == len(data)
                and all(48 <= c < 123 for c in name)):
            continue
        pixels = np.frombuffer(data[16:16 + width * height], dtype=np.uint8).reshape(height, width)
        palette = load_palette(rom, palette_of[at]) if at in palette_of else None
        if palette is not None:
            Image.fromarray(palette[pixels], "RGB").save(
                os.path.join(out, f"{name.decode()}_{width}x{height}_rom{at:07X}_pal{palette_of[at]}.png"))
            coloured += 1
        else:
            Image.fromarray(pixels, "L").save(os.path.join(out, f"{name.decode()}_{width}x{height}_rom{at:07X}.png"))
        count += 1
    print(f"{count} named 8-bit images written to {out} ({coloured} in colour)")


if __name__ == "__main__":
    main()
