# Usage: python scripts\who_calls.py <start_hex> <end_hex>
# Lists functions in [start, end) and who references them (jal, or a pointer in ROM data).
import os
import re, struct, sys

BASE = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))  # project folder (scripts\..)
ROM = BASE + r"\rom\game.z64"
SYM = BASE + r"\symbols.toml"
lo, hi = int(sys.argv[1], 16), int(sys.argv[2], 16)

rom = open(ROM, "rb").read()
secs, funcs = [], []
for p in re.split(r"(?m)^\[\[section\]\]\s*$", open(SYM).read())[1:]:
    r = int(re.search(r"rom = (0x[0-9A-Fa-f]+)", p).group(1), 16)
    v = int(re.search(r"vram = (0x[0-9A-Fa-f]+)", p).group(1), 16)
    sz = int(re.search(r"(?m)^size = (0x[0-9A-Fa-f]+)", p).group(1), 16)
    fs = [(n, int(a, 16), int(b, 16)) for n, a, b in
          re.findall(r'name = "([^"]+)", vram = (0x[0-9A-Fa-f]+), size = (0x[0-9A-Fa-f]+) \}', p)]
    secs.append((r, v, sz, fs))
    funcs += fs

inside = {a: (n, s) for n, a, s in funcs if lo <= a < hi}
jal = {a: [] for a in inside}
ptr = {a: [] for a in inside}
for r, v, sz, fs in secs:
    words = struct.unpack(">%dI" % (sz // 4), rom[r:r + sz // 4 * 4])
    owner = sorted(fs, key=lambda f: f[1])
    for i, w in enumerate(words):
        pc = v + i * 4
        if w in ptr:
            ptr[w].append(pc)
        if w >> 26 == 3:
            t = (pc & 0xF0000000) | ((w & 0x3FFFFFF) << 2)
            if t in jal:
                caller = next((n for n, a, s in owner if a <= pc < a + s), f"{pc:08X}")
                jal[t].append(caller)

for a in sorted(inside):
    n, s = inside[a]
    print(f"{n} @ {a:08X} size 0x{s:X}: jal callers {sorted(set(jal[a]))[:6] or 'none'}, "
          f"pointer refs {[f'{x:08X}' for x in ptr[a][:4]] or 'none'}")
