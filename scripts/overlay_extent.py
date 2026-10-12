# Usage: python scripts\overlay_extent.py <ramdump.bin> <ram_addr> <rom_offset>
# Prints runs of bytes in RAM that match the ROM at the same offset difference.
import os
import sys

ROM = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "rom", "game.z64")
ram = open(sys.argv[1], "rb").read()
ram_addr, rom_off = int(sys.argv[2], 16), int(sys.argv[3], 16)
rom = open(ROM, "rb").read()

delta = rom_off - (ram_addr & 0x7FFFFF)
lo, hi = 0x300000, 0x400000

runs, start = [], None
for off in range(lo, hi):
    r = off + delta
    ok = 0 <= r < len(rom) and rom[r] == ram[off]
    if ok and start is None:
        start = off
    elif not ok and start is not None:
        runs.append((start, off)); start = None
if start is not None:
    runs.append((start, hi))

for s, e in runs:
    if e - s >= 0x40:
        print(f"RAM {0x80000000 + s:08X}-{0x80000000 + e:08X}  ROM 0x{s + delta:X}-0x{e + delta:X}  (0x{e - s:X} bytes)")
