# Usage: python scripts\find_overlay.py <ramdump.bin>
# Looks for code loaded near the top of RAM and finds where it came from in the ROM.
import os
import sys
from collections import Counter

ROM = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "rom", "game.z64")
LO, HI = 0x80300000, 0x80400000
STEP, LEN = 0x100, 32
WANT = 0x8039691C   # the call target the recompiler is missing

rom = open(ROM, "rb").read()
ram = open(sys.argv[1], "rb").read()

nonzero = [a for a in range(LO, HI, STEP) if any(ram[a & 0x7FFFFF:(a & 0x7FFFFF) + LEN])]
print(f"non-zero chunks in {LO:08X}-{HI:08X}: {len(nonzero)} of {(HI - LO) // STEP}")
o = WANT & 0x7FFFFF
print(f"bytes at {WANT:08X}: {ram[o:o+16].hex(' ')}")

deltas = Counter()
first = {}
for a in nonzero:
    chunk = ram[a & 0x7FFFFF:(a & 0x7FFFFF) + LEN]
    i = rom.find(chunk)
    if i < 0 or rom.find(chunk, i + 1) >= 0:
        continue
    d = i - (a & 0x7FFFFF)
    deltas[d] += 1
    first.setdefault(d, a)

if not deltas:
    print("no ROM matches (empty, compressed, or generated at runtime)")
for d, n in deltas.most_common(5):
    a = first[d]
    print(f"{n:4d} matching chunks: RAM {a:08X} <- ROM offset 0x{(a & 0x7FFFFF) + d:X}")
