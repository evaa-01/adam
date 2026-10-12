# Usage: python scripts\find_code_end.py [section]      (default .text2)
# Shows, per 0x1000 block, how many functions look like data (invalid CPU opcodes,
# or jal to an address that is not a known function), and suggests where code ends.
import os
import re, struct, sys
from collections import defaultdict

BASE = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))  # project folder (scripts\..)
ROM = BASE + r"\rom\game.z64"
SYM = BASE + r"\symbols.toml"
want = sys.argv[1] if len(sys.argv) > 1 else ".text2"
BLOCK = 0x1000
# COP2/COP3 and other opcodes the N64 CPU never uses in game code
BAD = {0x12, 0x13, 0x1C, 0x1D, 0x1E, 0x1F, 0x32, 0x33, 0x36, 0x3A, 0x3B, 0x3E}

rom = open(ROM, "rb").read()
secs, starts = {}, set()
for p in re.split(r"(?m)^\[\[section\]\]\s*$", open(SYM).read())[1:]:
    n = re.search(r'name = "([^"]+)"', p).group(1)
    r = int(re.search(r"rom = (0x[0-9A-Fa-f]+)", p).group(1), 16)
    v = int(re.search(r"vram = (0x[0-9A-Fa-f]+)", p).group(1), 16)
    fs = [(fn, int(a, 16), int(b, 16)) for fn, a, b in
          re.findall(r'name = "([^"]+)", vram = (0x[0-9A-Fa-f]+), size = (0x[0-9A-Fa-f]+) \}', p)]
    secs[n] = (r, v, fs)
    starts.update(a for _, a, _ in fs)
if want not in secs:
    sys.exit(f"section {want} not found; have {list(secs)}")

r, base, fs = secs[want]
rows = defaultdict(lambda: [0, 0])
flags = []
for fn, a, s in sorted(fs, key=lambda f: f[1]):
    o = r + (a - base)
    words = struct.unpack(">%dI" % (s // 4), rom[o:o + s])
    why = None
    for i, w in enumerate(words):
        op = w >> 26
        if op in BAD:
            why = f"opcode 0x{op:02X}"; break
        if op == 3:
            t = ((a + i * 4) & 0xF0000000) | ((w & 0x3FFFFFF) << 2)
            if t not in starts:
                why = f"jal {t:08X}"; break
    blk = a - (a - base) % BLOCK
    rows[blk][0] += 1
    if why:
        rows[blk][1] += 1
    flags.append((fn, a, s, why))

print(f"{want}: block     funcs  suspicious")
for b in sorted(rows):
    n, k = rows[b]
    print(f"  {b:08X}  {n:5d}  {k:5d}  {'#' * k}")

# suggest: earliest suspicious function after which >= 70% of the rest are suspicious
sug = None
for i, (fn, a, s, why) in enumerate(flags):
    if not why:
        continue
    rest = flags[i:]
    if sum(1 for f in rest if f[3]) >= 0.7 * len(rest):
        sug = (fn, a, why)
        break
print("\nsuspicious functions:")
for fn, a, s, why in flags:
    if why:
        print(f"  {fn} @ {a:08X}: {why}")
if sug:
    print(f"\nsuggested code end: {sug[1]:08X} (first of the data-like tail: {sug[0]}, {sug[2]})")
    print(f"command: python scripts\\split_data.py {want} {sug[1]:08X}")
else:
    print("\nno clear data tail; suspicious functions are scattered, look at them individually")