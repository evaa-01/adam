# Usage: python scripts\inspect_func.py <function_name>
# Finds a function's real end (covering all its forward branches) and says whether
# merging is safe or whether a size override (overlap) is needed.
import os
import re, struct, sys
from collections import defaultdict

BASE = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))  # project folder (scripts\..)
ROM = BASE + r"\rom\game.z64"
SYM = BASE + r"\symbols.toml"
target = sys.argv[1]

rom = open(ROM, "rb").read()
text = open(SYM).read()

secs = []
for p in re.split(r"(?m)^\[\[section\]\]\s*$", text)[1:]:
    r = int(re.search(r"rom = (0x[0-9A-Fa-f]+)", p).group(1), 16)
    v = int(re.search(r"vram = (0x[0-9A-Fa-f]+)", p).group(1), 16)
    sz = int(re.search(r"(?m)^size = (0x[0-9A-Fa-f]+)", p).group(1), 16)
    fs = [(n, int(a, 16), int(b, 16)) for n, a, b in
          re.findall(r'name = "([^"]+)", vram = (0x[0-9A-Fa-f]+), size = (0x[0-9A-Fa-f]+) \}', p)]
    secs.append((r, v, sz, fs))

def word(r, v, a):
    o = r + (a - v)
    return struct.unpack(">I", rom[o:o + 4])[0]

callers = defaultdict(list)
for r, v, sz, fs in secs:
    for n, a, s in fs:
        for i in range(s // 4):
            w = word(r, v, a + i * 4)
            if w >> 26 == 3:
                callers[((a + i * 4) & 0xF0000000) | ((w & 0x3FFFFFF) << 2)].append(n)

def btarget(pc, w):
    op, rs, rt = w >> 26, (w >> 21) & 31, (w >> 16) & 31
    if not (op in (4, 5, 6, 7, 20, 21, 22, 23) or (op == 1 and rt in (0, 1, 2, 3)) or (op == 17 and rs == 8)):
        return None
    imm = w & 0xFFFF
    return pc + 4 + ((imm - 0x10000) if imm & 0x8000 else imm) * 4

starts = {a for _, _, _, fs in secs for _, a, _ in fs}
found = False
for r, v, sz, fs in secs:
    for n, a, s in fs:
        if n != target:
            continue
        found = True
        top = v + sz
        need, p, end = a + s, a, None
        while p < top:
            w = word(r, v, p)
            t = btarget(p, w)
            if t is not None and p < t < top:
                need = max(need, t + 4)
            end_here = (w == 0x03E00008)
            if w >> 26 == 2:            # j: tail call only if it lands on a function start
                jt = (p & 0xF0000000) | ((w & 0x3FFFFFF) << 2)
                if jt in starts and jt != a:
                    end_here = True
                elif p < jt < top:
                    need = max(need, jt + 4)
            if p + 8 >= need and end_here:
                end = p + 8
                break
            p += 4
        if end is None:
            print(f"{n}: no end found before section end, look at it by hand")
            sys.exit(1)
        print(f"{n}: start {a:08X}, current size 0x{s:X}, real end {end:08X}, new size 0x{end - a:X}")
        swallowed = [(m, b) for m, b, _ in fs if a < b < end]
        bad = False
        for m, b in swallowed:
            ext = sorted({x for x in callers.get(b, []) if x != n and x not in {y for y, _ in swallowed}})
            if ext:
                bad = True
            print(f"  overlaps {m} @ {b:08X}: {'called from elsewhere ' + str(ext[:5]) if ext else 'not called elsewhere'}")
        print(f"\nadd to scripts\\size_overrides.txt:\n{n} 0x{end - a:X}")
        if bad:
            print("(overlap is intended: the other function stays and the shared code is translated twice)")
if not found:
    print(f"{target} not found in symbols.toml")